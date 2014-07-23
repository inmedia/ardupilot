// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
  handle vehicle <-> GCS communications for terrain library
 */

#include <AP_HAL.h>
#include <AP_Common.h>
#include <AP_Math.h>
#include <GCS_MAVLink.h>
#include <GCS.h>
#include "AP_Terrain.h"

#if HAVE_AP_TERRAIN

#include <assert.h>
#include <stdio.h>

extern const AP_HAL::HAL& hal;

/*
  request any missing 4x4 grids from a block, given a grid_cache
 */
bool AP_Terrain::request_missing(mavlink_channel_t chan, struct grid_cache &gcache)
{
    struct grid_block &grid = gcache.grid;

    // see if we are waiting for disk read
    if (gcache.state == GRID_CACHE_DISKWAIT) {
        // don't request data from the GCS till we know its not on disk
        return false;
    }

    // see if it is fully populated
    if ((grid.bitmap & bitmap_mask) == bitmap_mask) {
        // it is fully populated, nothing to do
        return false;
    }

    /*
      ask the GCS to send a set of 4x4 grids
     */
    mavlink_msg_terrain_request_send(chan, grid.lat, grid.lon, grid_spacing, bitmap_mask & ~grid.bitmap);
    last_request_time_ms = hal.scheduler->millis();

    return true;
}

/*
  request any missing 4x4 grids from a block
 */
bool AP_Terrain::request_missing(mavlink_channel_t chan, const struct grid_info &info)
{
    // find the grid
    struct grid_cache &gcache = find_grid(info);
    return request_missing(chan, gcache);
}

/*
  send any pending terrain request to the GCS
 */
void AP_Terrain::send_request(mavlink_channel_t chan)
{
    if (enable == 0) {
        // not enabled
        return;
    }

    // see if we need to schedule some disk IO
    schedule_disk_io();

    // did we request recently?
    if (hal.scheduler->millis() - last_request_time_ms < 2000) {
        // too soon to request again
        return;
    }

    Location loc;
    if (!ahrs.get_position(loc)) {
        // we don't know where we are
        return;
    }

    // request any missing 4x4 blocks in the current grid
    struct grid_info info;
    calculate_grid_info(loc, info);

    if (request_missing(chan, info)) {
        return;
    }

    // also request a larger set of up to 9 grids
    for (int8_t x=-1; x<=1; x++) {
        for (int8_t y=-1; y<=1; y++) {
            Location loc2 = loc;
            location_offset(loc2, 
                            x*TERRAIN_GRID_BLOCK_SIZE_X*0.7f*grid_spacing,
                            y*TERRAIN_GRID_BLOCK_SIZE_Y*0.7f*grid_spacing);
            struct grid_info info2;
            calculate_grid_info(loc2, info2);            
            if (request_missing(chan, info2)) {
                return;
            }
        }
    }

    // check cache blocks that may have been setup by a TERRAIN_CHECK
    for (uint16_t i=0; i<TERRAIN_GRID_BLOCK_CACHE_SIZE; i++) {
        if (cache[i].state >= GRID_CACHE_VALID) {
            if (request_missing(chan, cache[i])) {
                return;
            }
        }
    }

    // request the current loc last to ensure it has highest last
    // access time
    if (request_missing(chan, info)) {
        return;
    }

    // nothing to request, send a terrain report
    send_terrain_report(chan, loc);
}

/*
  count bits in a uint64_t
*/
uint8_t AP_Terrain::bitcount64(uint64_t b)
{
    return __builtin_popcount((unsigned)(b&0xFFFFFFFF)) + __builtin_popcount((unsigned)(b>>32));
}

/*
  get some statistics for TERRAIN_REPORT
*/
void AP_Terrain::get_statistics(uint16_t &pending, uint16_t &loaded)
{
    pending = 0;
    loaded = 0;
    for (uint16_t i=0; i<TERRAIN_GRID_BLOCK_CACHE_SIZE; i++) {
        if (cache[i].state == GRID_CACHE_INVALID) {
            continue;
        }
        uint8_t maskbits = TERRAIN_GRID_BLOCK_MUL_X*TERRAIN_GRID_BLOCK_MUL_Y;
        if (cache[i].state == GRID_CACHE_DISKWAIT) {
            pending += maskbits;
            continue;
        }
        uint8_t bitcount = bitcount64(cache[i].grid.bitmap);
        pending += maskbits - bitcount;
        loaded += bitcount;
    }
}


/* 
   handle terrain messages from GCS
 */
void AP_Terrain::handle_data(mavlink_channel_t chan, mavlink_message_t *msg)
{
    if (msg->msgid == MAVLINK_MSG_ID_TERRAIN_DATA) {
        handle_terrain_data(msg);
    } else if (msg->msgid == MAVLINK_MSG_ID_TERRAIN_CHECK) {
        handle_terrain_check(chan, msg);
    }
}


/* 
   send a TERRAIN_REPORT for a location
 */
void AP_Terrain::send_terrain_report(mavlink_channel_t chan, const Location &loc)
{
    float height = 0;
    uint16_t spacing = 0;
    if (height_amsl(loc, height)) {
        spacing = grid_spacing;
    }
    uint16_t pending, loaded;
    get_statistics(pending, loaded);
    if (comm_get_txspace(chan) >= MAVLINK_NUM_NON_PAYLOAD_BYTES + MAVLINK_MSG_ID_TERRAIN_REPORT_LEN) {
        mavlink_msg_terrain_report_send(chan, loc.lat, loc.lng, spacing, height, pending, loaded);
    }
}

/* 
   handle TERRAIN_CHECK messages from GCS
 */
void AP_Terrain::handle_terrain_check(mavlink_channel_t chan, mavlink_message_t *msg)
{
    mavlink_terrain_check_t packet;
    mavlink_msg_terrain_check_decode(msg, &packet);
    Location loc;
    loc.lat = packet.lat;
    loc.lng = packet.lon;
    send_terrain_report(chan, loc);
}

/* 
   handle TERRAIN_DATA messages from GCS
 */
void AP_Terrain::handle_terrain_data(mavlink_message_t *msg)
{
    mavlink_terrain_data_t packet;
    mavlink_msg_terrain_data_decode(msg, &packet);

    uint16_t i;
    for (i=0; i<TERRAIN_GRID_BLOCK_CACHE_SIZE; i++) {
        if (cache[i].grid.lat == packet.lat && 
            cache[i].grid.lon == packet.lon && 
            cache[i].grid.spacing == packet.grid_spacing &&
            packet.gridbit < 56) {
            break;
        }
    }
    if (i == TERRAIN_GRID_BLOCK_CACHE_SIZE) {
        // we don't have that grid, ignore data
        return;
    }
    struct grid_cache &gcache = cache[i];
    struct grid_block &grid = gcache.grid;
    uint8_t idx_x = (packet.gridbit / TERRAIN_GRID_BLOCK_MUL_Y) * TERRAIN_GRID_MAVLINK_SIZE;
    uint8_t idx_y = (packet.gridbit % TERRAIN_GRID_BLOCK_MUL_Y) * TERRAIN_GRID_MAVLINK_SIZE;
    ASSERT_RANGE(idx_x,0,(TERRAIN_GRID_BLOCK_MUL_X-1)*TERRAIN_GRID_MAVLINK_SIZE);
    ASSERT_RANGE(idx_y,0,(TERRAIN_GRID_BLOCK_MUL_Y-1)*TERRAIN_GRID_MAVLINK_SIZE);
    for (uint8_t x=0; x<TERRAIN_GRID_MAVLINK_SIZE; x++) {
        for (uint8_t y=0; y<TERRAIN_GRID_MAVLINK_SIZE; y++) {
            grid.height[idx_x+x][idx_y+y] = packet.data[x*TERRAIN_GRID_MAVLINK_SIZE+y];
            ASSERT_RANGE(grid.height[idx_x+x][idx_y+y], 1, 20000);
        }
    }
    gcache.grid.bitmap |= ((uint64_t)1) << packet.gridbit;
    
    // mark dirty for disk IO
    gcache.state = GRID_CACHE_DIRTY;
    
#if TERRAIN_DEBUG
    hal.console->printf("Filled bit %u idx_x=%u idx_y=%u\n", 
                        (unsigned)packet.gridbit, (unsigned)idx_x, (unsigned)idx_y);
    if (gcache.grid.bitmap == bitmap_mask) {
        hal.console->printf("--lat=%12.7f --lon=%12.7f %u\n",
                            grid.lat*1.0e-7f,
                            grid.lon*1.0e-7f,
                            grid.height[0][0]);
        Location loc2;
        loc2.lat = grid.lat;
        loc2.lng = grid.lon;
        location_offset(loc2, 28*grid_spacing, 32*grid_spacing);
        hal.console->printf("--lat=%12.7f --lon=%12.7f %u\n",
                            loc2.lat*1.0e-7f,
                            loc2.lng*1.0e-7f,
                            grid.height[27][31]);            
    }
#endif
    
    // see if we need to schedule some disk IO
    update();
}


#endif // HAVE_AP_TERRAIN
