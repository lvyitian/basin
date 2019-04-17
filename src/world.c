/*
 * world.c
 *
 *  Created on: Feb 22, 2016
 *      Author: root
 */

#include "basin/network.h"
#include "basin/packet.h"
#include <basin/game.h>
#include <basin/block.h>
#include <basin/tileentity.h>
#include <basin/entity.h>
#include <basin/world.h>
#include <basin/globals.h>
#include <basin/profile.h>
#include <basin/server.h>
#include <basin/ai.h>
#include <basin/plugin.h>
#include <basin/perlin.h>
#include <basin/biome.h>
#include <basin/entity.h>
#include <basin/player.h>
#include <basin/nbt.h>
#include <basin/worldmanager.h>
#include <avuna/prqueue.h>
#include <avuna/string.h>
#include <avuna/pmem.h>
#include <avuna/queue.h>
#include <avuna/hash.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <zlib.h>
#include <dirent.h>
#include <math.h>
#include <errno.h>


struct chunk* world_get_chunk(struct world* world, int32_t x, int32_t z) {
	struct chunk* ch = hashmap_getint(world->chunks, chunk_get_key_direct(x, z));
	return ch;
}

struct chunk* world_get_chunk_guess(struct world* world, struct chunk* ch, int32_t x, int32_t z) {
	if (ch == NULL) return world_get_chunk(world, x, z);
	if (ch->x == x && ch->z == z) return ch;
	if (abs(x - ch->x) > 3 || abs(z - ch->z) > 3) return world_get_chunk(world, x, z);
	struct chunk* cch = ch;
	while (cch != NULL) {
		if (cch->x > x) cch = cch->xn;
		else if (cch->x < x) cch = cch->xp;
		if (cch != NULL) {
			if (cch->z > z) cch = cch->zn;
			else if (cch->z < z) cch = cch->zp;
		}
		if (cch != NULL && cch->x == x && cch->z == z) return cch;
	}
	return world_get_chunk(world, x, z);
}


// WARNING: you almost certainly do not want to call this function. (hence not present in header)
// chunks are loaded asynchronously
struct chunk* world_load_chunk(struct world* world, int32_t x, int32_t z, size_t chri) {
	struct chunk* chunk = hashmap_getint(world->chunks, chunk_get_key_direct(x, z));
	if (chunk != NULL) return chunk;
	int16_t region_x = (int16_t) (x >> 5);
	int16_t region_z = (int16_t) (z >> 5);
	uint64_t region_index = (((uint64_t)(region_x) & 0xFFFF) << 16) | (((uint64_t) region_z) & 0xFFFF);
	struct region* region = hashmap_getint(world->regions, region_index);
	if (region == NULL) {
		char path[PATH_MAX];
		snprintf(path, PATH_MAX, "%s/region/r.%i.%i.mca", world->world_folder, region_x, region_z);
		region = region_new(path, region_x, region_z);
		hashmap_putint(world->regions, region_index, region);
	}
	chunk = region_load_chunk(region, x & 0x1F, z & 0x1F);
	if (chunk == NULL) {
		chunk = chunk_new(x, z);
		generateChunk(world, chunk);
	}
	if (chunk != NULL) {
		chunk->xp = world_get_chunk(world, x + 1, z);
		if (chunk->xp != NULL) chunk->xp->xn = chunk;
		chunk->xn = world_get_chunk(world, x - 1, z);
		if (chunk->xn != NULL) chunk->xn->xp = chunk;
		chunk->zp = world_get_chunk(world, x, z + 1);
		if (chunk->zp != NULL) chunk->zp->zn = chunk;
		chunk->zn = world_get_chunk(world, x, z - 1);
		if (chunk->zn != NULL) chunk->zn->zp = chunk;
		hashmap_putint(world->chunks, chunk_get_key(chunk), chunk);
		return chunk;
	}
	return gc;
}

void chunkloadthr(size_t b) {
	//TODO: ensure that on world change that the chunk queue is cleared for a player
	while (1) {
		nm: ;
		pthread_mutex_lock (&chunk_wake_mut);
		pthread_cond_wait(&chunk_wake, &chunk_wake_mut);
		pthread_mutex_unlock(&chunk_wake_mut);
		if (players->entry_count == 0 && globalChunkQueue->size == 0) goto nm;
		if (globalChunkQueue->size > 0) {
			struct chunk_request* cr = NULL;
			while ((cr = pop_nowait_queue(globalChunkQueue)) != NULL) {
				if (cr->load) {
					beginProfilerSection("chunkLoading_getChunk");
					struct chunk* ch = world_load_chunk(cr->world, cr->cx, cr->cz, b);
					if (ch != NULL) ch->playersLoaded++;
					endProfilerSection("chunkLoading_getChunk");
				} else {
					struct chunk* ch = world_get_chunk(cr->world, cr->cx, cr->cz);
					if (ch != NULL && !ch->defunct) {
						if (--ch->playersLoaded <= 0) {
							world_unload_chunk(cr->world, ch);
						}
					}
				}
			}
		}
		BEGIN_HASHMAP_ITERATION (players)
		struct player* player = value;
		if (player->defunct || player->chunksSent >= 5 || player->chunkRequests->size == 0 || (player->conn != NULL && player->conn->writeBuffer_size > 1024 * 1024 * 128)) continue;
		struct chunk_request* chr = pop_nowait_queue(player->chunkRequests);
		if (chr == NULL) continue;
		if (chr->load) {
			if (chr->world != player->world) {
				xfree(chr);
				continue;
			}
			player->chunksSent++;
			if (contains_hashmap(player->loadedChunks, chunk_get_key_direct(chr->cx, chr->cz))) {
				xfree(chr);
				continue;
			}
			beginProfilerSection("chunkLoading_getChunk");
			struct chunk* ch = world_load_chunk(player->world, chr->cx, chr->cz, b);
			if (player->loadedChunks == NULL) {
				xfree(chr);
				endProfilerSection("chunkLoading_getChunk");
				continue;
			}
			endProfilerSection("chunkLoading_getChunk");
			if (ch != NULL) {
				ch->playersLoaded++;
				//beginProfilerSection("chunkLoading_sendChunk_add");
				put_hashmap(player->loadedChunks, chunk_get_key(ch), ch);
				//endProfilerSection("chunkLoading_sendChunk_add");
				//beginProfilerSection("chunkLoading_sendChunk_malloc");
				struct packet* pkt = xmalloc(sizeof(struct packet));
				pkt->id = PKT_PLAY_CLIENT_CHUNKDATA;
				pkt->data.play_client.chunkdata.data = ch;
				pkt->data.play_client.chunkdata.cx = ch->x;
				pkt->data.play_client.chunkdata.cz = ch->z;
				pkt->data.play_client.chunkdata.ground_up_continuous = 1;
				pkt->data.play_client.chunkdata.number_of_block_entities = ch->tileEntities->count;
				//endProfilerSection("chunkLoading_sendChunk_malloc");
				//beginProfilerSection("chunkLoading_sendChunk_tileEntities");
				pkt->data.play_client.chunkdata.block_entities = ch->tileEntities->count == 0 ? NULL : xmalloc(sizeof(struct nbt_tag*) * ch->tileEntities->count);
				size_t ri = 0;
				for (size_t i = 0; i < ch->tileEntities->size; i++) {
					if (ch->tileEntities->data[i] == NULL) continue;
					struct tile_entity* te = ch->tileEntities->data[i];
					pkt->data.play_client.chunkdata.block_entities[ri++] = serializeTileEntity(te, 1);
				}
				//endProfilerSection("chunkLoading_sendChunk_tileEntities");
				//beginProfilerSection("chunkLoading_sendChunk_dispatch");
				add_queue(player->outgoingPacket, pkt);
				flush_outgoing(player);
				//endProfilerSection("chunkLoading_sendChunk_dispatch");
			}
		} else {
			//beginProfilerSection("unchunkLoading");
			struct chunk* ch = world_get_chunk(player->world, chr->cx, chr->cz);
			uint64_t ck = chunk_get_key_direct(chr->cx, chr->cz);
			if (get_hashmap(player->loadedChunks, ck) == NULL) {
				xfree(chr);
				continue;
			}
			put_hashmap(player->loadedChunks, ck, NULL);
			if (ch != NULL && !ch->defunct) {
				if (--ch->playersLoaded <= 0) {
					world_unload_chunk(player->world, ch);
				}
			}
			if (chr->world == player->world) {
				struct packet* pkt = xmalloc(sizeof(struct packet));
				pkt->id = PKT_PLAY_CLIENT_UNLOADCHUNK;
				pkt->data.play_client.unloadchunk.chunk_x = chr->cx;
				pkt->data.play_client.unloadchunk.chunk_z = chr->cz;
				pkt->data.play_client.unloadchunk.ch = NULL;
				add_queue(player->outgoingPacket, pkt);
				flush_outgoing(player);
			}
			//endProfilerSection("unchunkLoading");
		}
		xfree(chr);
		END_HASHMAP_ITERATION (players)
	}
}

//TODO: ensure proper call locations
void world_unload_chunk(struct world* world, struct chunk* chunk) {
//TODO: save chunk
	pthread_rwlock_wrlock(&world->chunks->rwlock);
	if (chunk->xp != NULL) chunk->xp->xn = NULL;
	if (chunk->xn != NULL) chunk->xn->xp = NULL;
	if (chunk->zp != NULL) chunk->zp->zn = NULL;
	if (chunk->zn != NULL) chunk->zn->zp = NULL;
	pthread_rwlock_unlock(&world->chunks->rwlock);
	hashmap_putint(world->chunks, chunk_get_key(chunk), NULL);
	pfree(chunk->pool);
}

int world_get_biome(struct world* world, int32_t x, int32_t z) {
	struct chunk* chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return 0;
	return chunk->biomes[z & 0x0f][x & 0x0f];
}


uint8_t world_get_light_guess(struct world* world, struct chunk* ch, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return 0;
	ch = world_get_chunk_guess(world, ch, x >> 4, z >> 4);
	if (ch != NULL) return chunk_get_light(ch, (uint8_t) (x & 0x0f), (uint8_t) y, (uint8_t) (z & 0x0f), world->skylightSubtracted);
	else return world_get_light(world, x, y, z, 0);
}

uint8_t world_get_raw_light_guess(struct world* world, struct chunk* ch, int32_t x, int32_t y, int32_t z, uint8_t blocklight) {
	if (y < 0 || y > 255) return 0;
	ch = world_get_chunk_guess(world, ch, x >> 4, z >> 4);
	if (ch != NULL) return chunk_get_raw_light(ch, (uint8_t) (x & 0x0f), (uint8_t) y, (uint8_t) (z & 0x0f), blocklight);
	else return world_get_raw_light(world, x, y, z, blocklight);
}

uint16_t world_height_guess(struct world* world, struct chunk* ch, int32_t x, int32_t z) {
	if (world->dimension != OVERWORLD) return 0;
	ch = world_get_chunk_guess(world, ch, x >> 4, z >> 4);
	if (ch == NULL) return 0;
	return ch->heightMap[z & 0x0f][x & 0x0f];
}

void world_set_light(struct world* world, struct chunk* ch, uint8_t light, int32_t x, int32_t y, int32_t z, uint8_t blocklight) {
	if (y < 0 || y > 255) return;
	ch = world_get_chunk_guess(world, ch, x >> 4, z >> 4);
	if (ch != NULL) return chunk_set_light(ch, (uint8_t) (light & 0x0f), (uint8_t) (x & 0x0f), (uint8_t) y, (uint8_t) (z & 0x0f), blocklight, (uint8_t) (world->dimension == 0));
	else return world_set_light_guess(world, (uint8_t) (light & 0x0f), x, y, z, blocklight);
}

void world_set_light_guess(struct world* world, uint8_t light, int32_t x, int32_t y, int32_t z, uint8_t blocklight) {
	if (y < 0 || y > 255) return;
	struct chunk* chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return;
    chunk_set_light(chunk, (uint8_t) (light & 0x0f), (uint8_t) (x & 0x0f), (uint8_t) (y > 255 ? 255 : y), (uint8_t) (z & 0x0f), blocklight, (uint8_t) (world->dimension == 0));
}

uint8_t world_get_light(struct world* world, int32_t x, int32_t y, int32_t z, uint8_t checkNeighbors) {
	if (y < 0 || y > 255) return 0;
	struct chunk* chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return 15;
	if (checkNeighbors) {
		uint8_t yp = chunk_get_light(chunk, (uint8_t) (x & 0x0f), y, (uint8_t) (z & 0x0f), world->skylightSubtracted);
		uint8_t xp = world_get_light_guess(world, chunk, x + 1, y, z);
		uint8_t xn = world_get_light_guess(world, chunk, x - 1, y, z);
		uint8_t zp = world_get_light_guess(world, chunk, x, y, z + 1);
		uint8_t zn = world_get_light_guess(world, chunk, x, y, z - 1);
		if (xp > yp) yp = xp;
		if (xn > yp) yp = xn;
		if (zp > yp) yp = zp;
		if (zn > yp) yp = zn;
		return yp;
	} else if (y < 0) return 0;
	else {
		return chunk_get_light(chunk, (uint8_t) (x & 0x0f), (uint8_t) (y > 255 ? 255 : y), (uint8_t) (xz & 0x0f), world->skylightSubtracted);
	}
}

uint8_t world_get_raw_light(struct world* world, int32_t x, int32_t y, int32_t z, uint8_t blocklight) {
	if (y < 0 || y > 255) return 0;
	struct chunk* chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return 15;
	return chunk_get_raw_light(chunk, (uint8_t) (x & 0x0f), (uint8_t) (y > 255 ? 255 : y), (uint8_t) (z & 0x0f), blocklight);
}

block world_get_block(struct world* world, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return 0;
	struct chunk* chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return 0;
	return chunk_get_block(chunk, (uint8_t) (x & 0x0f), (uint8_t) y, (uint8_t) (z & 0x0f));
}

block world_get_block_guess(struct world* world, struct chunk* chunk, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return 0;
	chunk = world_get_chunk_guess(world, chunk, x >> 4, z >> 4);
	if (chunk != NULL) return chunk_get_block(chunk, (uint8_t) (x & 0x0f), (uint8_t) y, (uint8_t) (z & 0x0f));
	else return world_get_block(world, x, y, z);
}

int wrt_intermediate(double v1x, double v1y, double v1z, double v2x, double v2y, double v2z, double coord, int ct, double* rx, double* ry, double* rz) {
	double dx = v2x - v1x;
	double dy = v2y - v1y;
	double dz = v2z - v1z;
	double* mcd = NULL;
	double* mc = NULL;
	if (ct == 0) {
		mcd = &dx;
		mc = &v1x;
	} else if (ct == 1) {
		mcd = &dy;
		mc = &v1y;
	} else if (ct == 2) {
		mcd = &dz;
		mc = &v1z;
	}
	if ((*mcd) * (*mcd) < 1.0000000116860974e-7) return 1;
	else {
		double dc = (coord - (*mc)) / (*mcd);
		if (dc >= 0. && dc <= 1.) {
			*rx = v1x + dx * dc;
			*ry = v1y + dy * dc;
			*rz = v1z + dz * dc;
			return 0;
		} else return 1;
	}
}

int wrt_intersectsPlane(double rc1, double rc2, double minc1, double minc2, double maxc1, double maxc2) {
	return rc1 >= minc1 && rc1 <= maxc1 && rc2 >= minc2 && rc2 <= maxc2;
}

int world_isColliding(struct block_info* bi, int32_t x, int32_t y, int32_t z, double px, double py, double pz) {
	for (size_t i = 0; i < bi->boundingBox_count; i++) {
		struct boundingbox* bb = &bi->boundingBoxes[i];
		if (bb->minX + x < px && bb->maxX + x > px && bb->minY + y < py && bb->maxY + y > py && bb->minZ + z < pz && bb->maxZ + z > pz) return 1;
	}
	return 0;
}

int wrt_isClosest(double tx, double ty, double tz, double x1, double y1, double z1, double x2, double y2, double z2) {
	return (x1 - tx) * (x1 - tx) + (y1 - ty) * (y1 - ty) + (z1 - tz) * (z1 - tz) > (x2 - tx) * (x2 - tx) + (y2 - ty) * (y2 - ty) + (z2 - tz) * (z2 - tz);
}

int world_blockRayTrace(struct boundingbox* bb, int32_t x, int32_t y, int32_t z, double px, double py, double pz, double ex, double ey, double ez, double *qx, double* qy, double* qz) {
	double bx = ex;
	double by = ey;
	double bz = ez;
	double rx = 0.;
	double ry = 0.;
	double rz = 0.;
	int face = -1;
	if (!wrt_intermediate(px, py, pz, ex, ey, ez, bb->minX + x, 0, &rx, &ry, &rz) && wrt_intersectsPlane(ry, rz, bb->minY + y, bb->maxY + y, bb->minZ + z, bb->maxZ + z)) {
		face = XN;
		bx = rx;
		by = ry;
		bz = rz;
	}
	if (!wrt_intermediate(px, py, pz, ex, ey, ez, bb->maxX + x, 0, &rx, &ry, &rz) && wrt_intersectsPlane(ry, rz, bb->minX + x, bb->maxX + x, bb->minZ + z, bb->maxZ + z) && wrt_isClosest(px, py, pz, bx, by, bz, rx, ry, rz)) {
		face = XP;
		bx = rx;
		by = ry;
		bz = rz;
	}
	if (!wrt_intermediate(px, py, pz, ex, ey, ez, bb->minY + y, 1, &rx, &ry, &rz) && wrt_intersectsPlane(rx, rz, bb->minX + x, bb->maxX + x, bb->minZ + z, bb->maxZ + z) && wrt_isClosest(px, py, pz, bx, by, bz, rx, ry, rz)) {
		face = YN;
		bx = rx;
		by = ry;
		bz = rz;
	}
	if (!wrt_intermediate(px, py, pz, ex, ey, ez, bb->maxY + y, 1, &rx, &ry, &rz) && wrt_intersectsPlane(rx, rz, bb->minX + x, bb->maxX + x, bb->minZ + z, bb->maxZ + z) && wrt_isClosest(px, py, pz, bx, by, bz, rx, ry, rz)) {
		face = YP;
		bx = rx;
		by = ry;
		bz = rz;
	}
	if (!wrt_intermediate(px, py, pz, ex, ey, ez, bb->minZ + z, 2, &rx, &ry, &rz) && wrt_intersectsPlane(rx, ry, bb->minX + x, bb->maxX + x, bb->minY + y, bb->maxY + y) && wrt_isClosest(px, py, pz, bx, by, bz, rx, ry, rz)) {
		face = ZN;
		bx = rx;
		by = ry;
		bz = rz;
	}
	if (!wrt_intermediate(px, py, pz, ex, ey, ez, bb->maxZ + z, 2, &rx, &ry, &rz) && wrt_intersectsPlane(rx, ry, bb->minX + x, bb->maxX + x, bb->minY + y, bb->maxY + y) && wrt_isClosest(px, py, pz, bx, by, bz, rx, ry, rz)) {
		face = ZP;
		bx = rx;
		by = ry;
		bz = rz;
	}
	*qx = bx;
	*qy = by;
	*qz = bz;
	return face;
}

int world_rayTrace(struct world* world, double x, double y, double z, double ex, double ey, double ez, int stopOnLiquid, int ignoreNonCollidable, int returnLast, double* rx, double* ry, double* rz) {
	int32_t ix = (uint32_t) floor(x);
	int32_t iy = (uint32_t) floor(y);
	int32_t iz = (uint32_t) floor(z);
	int32_t iex = (uint32_t) floor(ex);
	int32_t iey = (uint32_t) floor(ey);
	int32_t iez = (uint32_t) floor(ez);
	block b = world_get_block(world, ix, iy, iz);
	struct block_info* bbi = getBlockInfo(b);
	if (bbi != NULL && (!ignoreNonCollidable || bbi->boundingBox_count > 0)) {
		double bx = 0.;
		double by = 0.;
		double bz = 0.;
		for (size_t i = 0; i < bbi->boundingBox_count; i++) {
			struct boundingbox* bb = &bbi->boundingBoxes[i];
			int face = world_blockRayTrace(bb, ix, iy, iz, x, y, z, ex, ey, ez, rx, ry, rz);
			if (face >= 0) return face;
		}
	}
	int k = 200;
	int cface = -1;
	while (k-- >= 0) {
		if (ix == iex && iy == iey && iz == iez) {
			return returnLast ? cface : -1;
		}
		int hx = 1;
		int hy = 1;
		int hz = 1;
		double mX = 999.;
		double mY = 999.;
		double mZ = 999.;
		if (iex > ix) mX = (double) ix + 1.;
		else if (iex < ix) mX = (double) ix;
		else hx = 0;
		if (iey > iy) mY = (double) iy + 1.;
		else if (iey < iy) mY = (double) iy;
		else hy = 0;
		if (iez > iz) mZ = (double) iz + 1.;
		else if (iez < iz) mZ = (double) iz;
		else hz = 0;
		double ax = 999.;
		double ay = 999.;
		double az = 999.;
		double dx = ex - x;
		double dy = ey - y;
		double dz = ez - z;
		if (hx) ax = (mX - x) / dx;
		if (hy) ay = (mY - y) / dy;
		if (hz) az = (mZ - z) / dz;
		if (ax == 0.) ax = -1e-4;
		if (ay == 0.) ay = -1e-4;
		if (az == 0.) az = -1e-4;
		if (ax < ay && ax < az) {
			cface = iex > ix ? XN : XP;
			x = mX;
			y += dy * ax;
			z += dz * ax;
		} else if (ay < az) {
			cface = iey > iy ? YN : YP;
			x += dx * ay;
			y = mY;
			z += dz * ay;
		} else {
			cface = iez > iz ? ZN : ZP;
			x += dx * az;
			y += dy * az;
			z = mZ;
		}
		ix = floor(x) - (cface == XP ? 1 : 0);
		iy = floor(y) - (cface == YP ? 1 : 0);
		iz = floor(z) - (cface == ZP ? 1 : 0);
		block nb = world_get_block(world, ix, iy, iz);
		struct block_info* bi = getBlockInfo(nb);
		if (bi != NULL && (!ignoreNonCollidable || str_eq(bi->material->name, "portal") || bi->boundingBox_count > 0)) {
			//todo: cancollidecheck?
			for (size_t i = 0; i < bi->boundingBox_count; i++) {
				struct boundingbox* bb = &bi->boundingBoxes[i];
				int face = world_blockRayTrace(bb, ix, iy, iz, x, y, z, ex, ey, ez, rx, ry, rz);
				if (face >= 0) return face;
			}
			//TODO: returnlast finish
		}
	}
	return returnLast ? cface : -1;
}

void world_explode(struct world* world, struct chunk* ch, double x, double y, double z, float strength) { // TODO: more plugin stuff?
	ch = world_get_chunk_guess(world, ch, (int32_t) floor(x) >> 4, (int32_t) floor(z) >> 4);
	for (int32_t j = 0; j < 16; j++) {
		for (int32_t k = 0; k < 16; k++) {
			for (int32_t l = 0; l < 16; l++) {
				if (!(j == 0 || j == 15 || k == 0 || k == 15 || l == 0 || l == 15)) continue;
				double dx = (double) j / 15.0 * 2.0 - 1.0;
				double dy = (double) k / 15.0 * 2.0 - 1.0;
				double dz = (double) l / 15.0 * 2.0 - 1.0;
				double d = sqrt(dx * dx + dy * dy + dz * dz);
				dx /= d;
				dy /= d;
				dz /= d;
				float mstr = strength * (.7 + randFloat() * .6);
				double x2 = x;
				double y2 = y;
				double z2 = z;
				for (; mstr > 0.; mstr -= .225) {
					int32_t ix = (int32_t) floor(x2);
					int32_t iy = (int32_t) floor(y2);
					int32_t iz = (int32_t) floor(z2);
					block b = world_get_block_guess(world, ch, ix, iy, iz);
					if (b >> 4 != 0) {
						struct block_info* bi = getBlockInfo(b);
						mstr -= ((bi->resistance / 5.) + .3) * .3;
						if (mstr > 0.) { //TODO: check if entity will allow destruction
							block b = world_get_block_guess(world, ch, ix, iy, iz);
							world_set_block_guess(world, ch, 0, ix, iy, iz);
							dropBlockDrops(world, b, NULL, ix, iy, iz); //TODO: randomizE?
						}
					}
					x2 += dx * .3;
					y2 += dy * .3;
					z2 += dz * .3;
				}
			}
		}
	}
	//TODO: knockback & damage
}

void world_tile_set_tickable(struct world* world, struct tile_entity* te) {
	if (te == NULL || te->y > 255 || te->y < 0) return;
	struct chunk* chunk = world_get_chunk(world, te->x >> 4, te->z >> 4);
	if (chunk == NULL) return;
	add_collection(chunk->tileEntitiesTickable, te);
}

void world_tile_unset_tickable(struct world* world, struct tile_entity* te) {
	if (te == NULL || te->y > 255 || te->y < 0) return;
	struct chunk* chunk = world_get_chunk(world, te->x >> 4, te->z >> 4);
	if (chunk == NULL) return;
	rem_collection(chunk->tileEntitiesTickable, te);
}

struct tile_entity* world_get_tile(struct world* world, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return NULL;
	struct chunk* chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return NULL;
	return chunk_get_tile(chunk, x, y, z);
}

void world_set_tile(struct world* world, int32_t x, int32_t y, int32_t z, struct tile_entity* te) {
	if (y < 0 || y > 255) return;
	struct chunk* chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return;
	chunk_set_tile(chunk, te, x, y, z);
}


void world_schedule_block_tick(struct world* world, int32_t x, int32_t y, int32_t z, int32_t ticksFromNow, float priority) {
	if (y < 0 || y > 255) return;
	struct scheduled_tick* st = xmalloc(sizeof(struct scheduled_tick));
	st->x = x;
	st->y = y;
	st->z = z;
	st->ticksLeft = ticksFromNow;
	st->priority = priority;
	st->src = world_get_block(world, x, y, z);
	struct encpos ep;
	ep.x = x;
	ep.y = y;
	ep.z = z;
	put_hashmap(world->scheduledTicks, *((uint64_t*) &ep), st);
}

int32_t world_is_block_tick_scheduled(struct world* world, int32_t x, int32_t y, int32_t z) {
	struct encpos ep;
	ep.x = x;
	ep.y = y;
	ep.z = z;
	struct scheduled_tick* st = get_hashmap(world->scheduledTicks, *((uint64_t*) &ep));
	if (st == NULL) {
		return 0;
	}
	return st->ticksLeft;
}

void world_doLightProc(struct world* world, struct chunk* chunk, int32_t x, int32_t y, int32_t z, uint8_t light) {
	uint8_t cl = world_get_raw_light_guess(world, chunk, x, y, z, 1);
	if (cl < light) world_set_light(world, chunk, light & 0x0f, x, y, z, 1);
}

struct world_lightpos {
		int32_t x;
		int32_t y;
		int32_t z;
};

int light_floodfill(struct world* world, struct chunk* chunk, struct world_lightpos* lp, int skylight, int subtract, struct hashmap* subt_upd) {
	if (lp->y < 0 || lp->y > 255) return 0;
	if (skylight && lp->y <= world_height_guess(world, chunk, lp->x, lp->z)) {
		struct block_info* bi = getBlockInfo(world_get_block_guess(world, chunk, lp->x, lp->y + 1, lp->z));
		if (bi != NULL && bi->lightOpacity <= 16) {
			struct world_lightpos lp2;
			lp2.x = lp->x;
			lp2.y = lp->y + 1;
			lp2.z = lp->z;
			light_floodfill(world, chunk, &lp2, skylight, subtract, subt_upd);
		}
	}
	struct block_info* bi = getBlockInfo(world_get_block_guess(world, chunk, lp->x, lp->y, lp->z));
	int lo = bi == NULL ? 1 : bi->lightOpacity;
	if (lo < 1) lo = 1;
	int le = bi == NULL ? 0 : bi->lightEmission;
	int16_t maxl = 0;
	uint8_t xpl = world_get_raw_light_guess(world, chunk, lp->x + 1, lp->y, lp->z, !skylight);
	uint8_t xnl = world_get_raw_light_guess(world, chunk, lp->x - 1, lp->y, lp->z, !skylight);
	uint8_t ypl = world_get_raw_light_guess(world, chunk, lp->x, lp->y + 1, lp->z, !skylight);
	uint8_t ynl = world_get_raw_light_guess(world, chunk, lp->x, lp->y - 1, lp->z, !skylight);
	uint8_t zpl = world_get_raw_light_guess(world, chunk, lp->x, lp->y, lp->z + 1, !skylight);
	uint8_t znl = world_get_raw_light_guess(world, chunk, lp->x, lp->y, lp->z - 1, !skylight);
	if (subtract) {
		maxl = world_get_raw_light_guess(world, chunk, lp->x, lp->y, lp->z, !skylight) - subtract;
	} else {
		maxl = xpl;
		if (xnl > maxl) maxl = xnl;
		if (ypl > maxl) maxl = ypl;
		if (ynl > maxl) maxl = ynl;
		if (zpl > maxl) maxl = zpl;
		if (znl > maxl) maxl = znl;
	}
	if (!subtract) maxl -= lo;
	else maxl += lo;
//printf("%s %i at %i, %i, %i\n", subtract ? "subtract" : "add", maxl, lp->x, lp->y, lp->z);
//if (maxl < 15) {
	int ks = 0;
	if ((subtract - lo) > 0) {
		subtract -= lo;
	} else {
		ks = 1;
	}
//}
	int sslf = 0;
	if (skylight) {
		int hm = world_height_guess(world, chunk, lp->x, lp->z);
		if (lp->y >= hm) {
			maxl = 15;
			if (subtract) sslf = 1;
		}
		//else maxl = -1;
	}
	if (maxl < le && !skylight) maxl = le;
	if (maxl < 0) maxl = 0;
	if (maxl > 15) maxl = 15;
	uint8_t pl = world_get_raw_light_guess(world, chunk, lp->x, lp->y, lp->z, !skylight);
	if (pl == maxl) return sslf;
	world_set_light(world, chunk, maxl, lp->x, lp->y, lp->z, !skylight);
	if (ks) return 1;
	if (subtract ? (maxl < 15) : (maxl > 0)) {

		if (subtract ? xpl > maxl : xpl < maxl) {
			lp->x++;
			if (light_floodfill(world, chunk, lp, skylight, subtract, subt_upd) && subtract) {
				lp->x--;
				void* n = xcopy(lp, sizeof(struct world_lightpos), 0);
				put_hashmap(subt_upd, (uint64_t) n, n);
				//light_floodfill(world, chunk, lp, skylight, 0, 0);
			} else lp->x--;
		}
		if (subtract ? xnl > maxl : xnl < maxl) {
			lp->x--;
			if (light_floodfill(world, chunk, lp, skylight, subtract, subt_upd) && subtract) {
				lp->x++;
				void* n = xcopy(lp, sizeof(struct world_lightpos), 0);
				put_hashmap(subt_upd, (uint64_t) n, n);
				//light_floodfill(world, chunk, lp, skylight, 0, 0);
			} else lp->x++;
		}
		if (!skylight && (subtract ? ypl > maxl : ypl < maxl)) {
			lp->y++;
			if (light_floodfill(world, chunk, lp, skylight, subtract, subt_upd) && subtract) {
				lp->y--;
				//light_floodfill(world, chunk, lp, skylight, 0, 0);
			} else lp->y--;
		}
		if (subtract ? ynl > maxl : ynl < maxl) {
			lp->y--;
			if (light_floodfill(world, chunk, lp, skylight, subtract, subt_upd) && subtract) {
				lp->y++;
				void* n = xcopy(lp, sizeof(struct world_lightpos), 0);
				put_hashmap(subt_upd, (uint64_t) n, n);
				//light_floodfill(world, chunk, lp, skylight, 0, 0);
			} else lp->y++;
		}
		if (subtract ? zpl > maxl : zpl < maxl) {
			lp->z++;
			if (light_floodfill(world, chunk, lp, skylight, subtract, subt_upd) && subtract) {
				lp->z--;
				void* n = xcopy(lp, sizeof(struct world_lightpos), 0);
				put_hashmap(subt_upd, (uint64_t) n, n);
				//light_floodfill(world, chunk, lp, skylight, 0, 0);
			} else lp->z--;
		}
		if (subtract ? znl > maxl : znl < maxl) {
			lp->z--;
			if (light_floodfill(world, chunk, lp, skylight, subtract, subt_upd) && subtract) {
				lp->z++;
				void* n = xcopy(lp, sizeof(struct world_lightpos), 0);
				put_hashmap(subt_upd, (uint64_t) n, n);
				//light_floodfill(world, chunk, lp, skylight, 0, 0);
			} else lp->z++;
		}
	}
	return sslf;
}

int world_set_block_guess(struct world* world, struct chunk* chunk, block blk, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return 1;
	chunk = world_get_chunk_guess(world, chunk, x >> 4, z >> 4);
	if (chunk == NULL) chunk = world_get_chunk(world, x >> 4, z >> 4);
	if (chunk == NULL) return 1;
	block ob = chunk_get_block(chunk, x & 0x0f, y, z & 0x0f);
	struct block_info* obi = getBlockInfo(ob);
	uint16_t ohm = world->dimension == OVERWORLD ? chunk->heightMap[z & 0x0f][x & 0x0f] : 0;
	struct world_lightpos lp;
	int pchm = 0;
	struct hashmap* nup = NULL;
	if (obi != NULL) {
		int ict = 0;
		if (ob != blk) {
			if (obi->onBlockDestroyed != NULL) ict = (*obi->onBlockDestroyed)(world, ob, x, y, z, blk);
			if (!ict) {
				BEGIN_HASHMAP_ITERATION (plugins)
				struct plugin* plugin = value;
				if (plugin->onBlockDestroyed != NULL && (*plugin->onBlockDestroyed)(world, ob, x, y, z, blk)) {
					ict = 1;
					BREAK_HASHMAP_ITERATION (plugins)
					break;
				}
				END_HASHMAP_ITERATION (plugins)
			}
			if (ict) return 1;
		}
		if (world->dimension == OVERWORLD && ((y >= ohm && obi->lightOpacity >= 1) || (y < ohm && obi->lightOpacity == 0))) {
			pchm = 1;
			nup = new_hashmap(1, 0);
			lp.x = x;
			lp.y = ohm;
			lp.z = z;
			light_floodfill(world, chunk, &lp, 1, 15, nup); // todo remove nup duplicates
		}
	}
	pnbi: ;
	struct block_info* nbi = getBlockInfo(blk);
	if (nbi != NULL && blk != ob) {
		int ict = 0;
		block obb = blk;
		if (nbi->onBlockPlaced != NULL) blk = (*nbi->onBlockPlaced)(world, blk, x, y, z, ob);
		if (blk == 0 && obb != 0) ict = 1;
		else if (blk != obb) goto pnbi;
		if (!ict) {
			BEGIN_HASHMAP_ITERATION (plugins)
			struct plugin* plugin = value;
			if (plugin->onBlockPlaced != NULL) {
				blk = (*plugin->onBlockPlaced)(world, blk, x, y, z, ob);
				if (blk == 0 && obb != 0) {
					ict = 1;
					BREAK_HASHMAP_ITERATION (plugins)
					break;
				} else if (blk != obb) goto pnbi;
			}
			END_HASHMAP_ITERATION (plugins)
		}
		if (ict) return 1;
	}
	chunk_set_block(chunk, blk, x & 0x0f, y, z & 0x0f, world->dimension == 0);
	uint16_t nhm = world->dimension == OVERWORLD ? chunk->heightMap[z & 0x0f][x & 0x0f] : 0;
	BEGIN_BROADCAST_DISTXYZ((double) x + .5, (double) y + .5, (double) z + .5, world->players, CHUNK_VIEW_DISTANCE * 16.)
	struct packet* pkt = xmalloc(sizeof(struct packet));
	pkt->id = PKT_PLAY_CLIENT_BLOCKCHANGE;
	pkt->data.play_client.blockchange.location.x = x;
	pkt->data.play_client.blockchange.location.y = y;
	pkt->data.play_client.blockchange.location.z = z;
	pkt->data.play_client.blockchange.block_id = blk;
	add_queue(bc_player->outgoingPacket, pkt);
	END_BROADCAST(world->players)
	beginProfilerSection("block_update");
	world_update_block_guess(world, chunk, x, y, z);
	world_update_block_guess(world, chunk, x + 1, y, z);
	world_update_block_guess(world, chunk, x - 1, y, z);
	world_update_block_guess(world, chunk, x, y, z + 1);
	world_update_block_guess(world, chunk, x, y, z - 1);
	world_update_block_guess(world, chunk, x, y + 1, z);
	world_update_block_guess(world, chunk, x, y - 1, z);
	endProfilerSection("block_update");
	beginProfilerSection("skylight_update");
	if (nbi == NULL || obi == NULL) return 0;
	if (world->dimension == OVERWORLD) {
		if (pchm || obi->lightOpacity != nbi->lightOpacity) {
			/*setLightWorld_guess(world, chunk, 15, x, nhm, z, 0);
			 struct world_lightpos lp;
			 if (ohm < nhm) {
			 for (int y = ohm; y < nhm; y++) {
			 setLightWorld_guess(world, chunk, 0, x, y, z, 0);
			 }
			 for (int y = ohm; y < nhm; y++) {
			 lp.x = x;
			 lp.y = y;
			 lp.z = z;
			 light_floodfill(world, chunk, &lp, 1);
			 }
			 } else {
			 for (int y = nhm; y < ohm; y++) {
			 world_set_light(world, chunk, 15, x, y, z, 0);
			 }
			 for (int y = nhm; y < ohm; y++) {
			 lp.x = x;
			 lp.y = y;
			 lp.z = z;
			 light_floodfill(world, chunk, &lp, 1);
			 }
			 }
			 lp.x = x;
			 lp.y = nhm;
			 lp.z = z;
			 light_floodfill(world, chunk, &lp, 1);*/

			beginProfilerSection("skylight_rst");
			/*for (int32_t lx = x - 16; lx <= x + 16; lx++) {
			 for (int32_t ly = ohm - 16; ly <= ohm + 16; ly++) {
			 for (int32_t lz = z - 16; lz <= z + 16; lz++) {
			 world_set_light(world, chunk, 0, lx, ly, lz, 0);
			 }
			 }
			 }*/
			/*for (int32_t lx = x - 16; lx <= x + 16; lx++) {
			 for (int32_t lz = z - 16; lz <= z + 16; lz++) {
			 int16_t hm = world_height_guess(world, chunk, lx, lz);
			 int16_t lb = ohm - 16;
			 int16_t ub = ohm + 16;
			 if (hm < ub && hm > lb) ub = hm;
			 for (int32_t ly = lb; ly <= ub; ly++) {
			 if (world_get_raw_light_guess(world, chunk, lx, ly, lz, 0) != 0) world_set_light(world, chunk, 0, lx, ly, lz, 0);
			 }
			 }
			 }*/
			//world_set_light(world, chunk, 0, x, ohm, z, 0);
			if (pchm) { // todo: remove the light before block change
				BEGIN_HASHMAP_ITERATION (nup)
				struct world_lightpos* nlp = value;
				light_floodfill(world, chunk, nlp, 1, 0, 0);
				xfree (value);
				END_HASHMAP_ITERATION (nup)
				del_hashmap(nup);
				//light_floodfill(world, chunk, &lp, 1, 0, 0);
				world_set_light(world, chunk, 15, x, nhm, z, 0);
				lp.x = x;
				lp.y = nhm;
				lp.z = z;
				light_floodfill(world, chunk, &lp, 1, 0, NULL);
			}
			lp.x = x;
			lp.y = y;
			lp.z = z;
			light_floodfill(world, chunk, &lp, 1, 0, NULL);
			endProfilerSection("skylight_rst");
			//TODO: pillar lighting?
			/*beginProfilerSection("skylight_set");
			 for (int32_t lz = z - 16; lz <= z + 16; lz++) {
			 for (int32_t lx = x - 16; lx <= x + 16; lx++) {
			 uint16_t hm = world_height_guess(world, chunk, lx, lz);
			 if (hm > 255) continue;
			 world_set_light(world, chunk, 15, lx, hm, lz, 0);
			 }
			 }
			 endProfilerSection("skylight_set");*/
			/*beginProfilerSection("skylight_fill");
			 for (int32_t lz = z - 16; lz <= z + 16; lz++) {
			 for (int32_t lx = x - 16; lx <= x + 16; lx++) {
			 uint16_t hm = world_height_guess(world, chunk, lx, lz);
			 if (hm > 255) continue;
			 lp.x = lx;
			 lp.y = hm;
			 lp.z = lz;
			 light_floodfill(world, chunk, &lp, 1, 0, 0);
			 }
			 }
			 endProfilerSection("skylight_fill");*/
		}
	}
	endProfilerSection("skylight_update");
	beginProfilerSection("blocklight_update");
	if (obi->lightEmission != nbi->lightEmission || obi->lightOpacity != nbi->lightOpacity) {
		if (obi->lightEmission <= nbi->lightEmission) {
			beginProfilerSection("blocklight_update_equals");
			struct world_lightpos lp;
			lp.x = x;
			lp.y = y;
			lp.z = z;
			light_floodfill(world, chunk, &lp, 0, 0, NULL);
			endProfilerSection("blocklight_update_equals");
		} else {
			beginProfilerSection("blocklight_update_remlight");
			nup = new_hashmap(1, 0);
			lp.x = x;
			lp.y = y;
			lp.z = z;
			light_floodfill(world, chunk, &lp, 0, obi->lightEmission, nup); // todo remove nup duplicates
			BEGIN_HASHMAP_ITERATION (nup)
			struct world_lightpos* nlp = value;
			light_floodfill(world, chunk, nlp, 0, 0, 0);
			xfree (value);
			END_HASHMAP_ITERATION (nup)
			del_hashmap(nup);
			//light_floodfill(world, chunk, &lp, 1, 0, 0);
			/*for (int32_t lx = x - 16; lx <= x + 16; lx++) {
			 for (int32_t ly = y - 16; ly <= y + 16; ly++) {
			 for (int32_t lz = z - 16; lz <= z + 16; lz++) {
			 world_set_light(world, chunk, 0, lx, ly, lz, 1);
			 }
			 }
			 }
			 for (int32_t lx = x - 16; lx <= x + 16; lx++) {
			 for (int32_t ly = y - 16; ly <= y + 16; ly++) {
			 struct world_lightpos lp;
			 lp.x = lx;
			 lp.y = ly;
			 lp.z = z - 16;
			 light_floodfill(world, chunk, &lp, 0, 0, NULL);
			 lp.z = z + 16;
			 light_floodfill(world, chunk, &lp, 0, 0, NULL);
			 }
			 }
			 for (int32_t lz = z - 16; lz <= z + 16; lz++) {
			 for (int32_t ly = y - 16; ly <= y + 16; ly++) {
			 struct world_lightpos lp;
			 lp.x = x - 16;
			 lp.y = ly;
			 lp.z = lz;
			 light_floodfill(world, chunk, &lp, 0, 0, NULL);
			 lp.x = x + 16;
			 light_floodfill(world, chunk, &lp, 0, 0, NULL);
			 }
			 }
			 for (int32_t lz = z - 16; lz <= z + 16; lz++) {
			 for (int32_t lx = x - 16; lx <= x + 16; lx++) {
			 struct world_lightpos lp;
			 lp.x = lx;
			 lp.y = y - 16;
			 lp.z = lz;
			 light_floodfill(world, chunk, &lp, 0, 0, NULL);
			 lp.y = y + 16;
			 light_floodfill(world, chunk, &lp, 0, 0, NULL);
			 }
			 }*/
			endProfilerSection("blocklight_update_remlight");
		}
	}
	endProfilerSection("blocklight_update");
	return 0;
}

int world_set_block_guess_noupdate(struct world* world, struct chunk* chunk, block blk, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return 1;
	chunk = world_get_chunk_guess(world, chunk, x >> 4, z >> 4);
	if (chunk == NULL) return 1;
	block ob = chunk_get_block(chunk, x & 0x0f, y, z & 0x0f);
	struct block_info* obi = getBlockInfo(ob);
	if (obi != NULL) {
		int ict = 0;
		if (ob != blk) {
			if (obi->onBlockDestroyed != NULL) ict = (*obi->onBlockDestroyed)(world, ob, x, y, z, blk);
			if (!ict) {
				BEGIN_HASHMAP_ITERATION (plugins)
				struct plugin* plugin = value;
				if (plugin->onBlockDestroyed != NULL && (*plugin->onBlockDestroyed)(world, ob, x, y, z, blk)) {
					ict = 1;
					BREAK_HASHMAP_ITERATION (plugins)
					break;
				}
				END_HASHMAP_ITERATION (plugins)
			}
			if (ict) return 1;
		}
	}
	pnbi: ;
	struct block_info* nbi = getBlockInfo(blk);
	if (nbi != NULL && blk != ob) {
		int ict = 0;
		block obb = blk;
		if (nbi->onBlockPlaced != NULL) blk = (*nbi->onBlockPlaced)(world, blk, x, y, z, ob);
		if (blk == 0 && obb != 0) ict = 1;
		else if (blk != obb) goto pnbi;
		if (!ict) {
			BEGIN_HASHMAP_ITERATION (plugins)
			struct plugin* plugin = value;
			if (plugin->onBlockPlaced != NULL) {
				blk = (*plugin->onBlockPlaced)(world, blk, x, y, z, ob);
				if (blk == 0 && obb != 0) {
					ict = 1;
					BREAK_HASHMAP_ITERATION (plugins)
					break;
				} else if (blk != obb) goto pnbi;
			}
			END_HASHMAP_ITERATION (plugins)
		}
		if (ict) return 1;
	}
	chunk_set_block(chunk, blk, x & 0x0f, y, z & 0x0f, world->dimension == 0);
	BEGIN_BROADCAST_DISTXYZ((double) x + .5, (double) y + .5, (double) z + .5, world->players, CHUNK_VIEW_DISTANCE * 16.)
	struct packet* pkt = xmalloc(sizeof(struct packet));
	pkt->id = PKT_PLAY_CLIENT_BLOCKCHANGE;
	pkt->data.play_client.blockchange.location.x = x;
	pkt->data.play_client.blockchange.location.y = y;
	pkt->data.play_client.blockchange.location.z = z;
	pkt->data.play_client.blockchange.block_id = blk;
	add_queue(bc_player->outgoingPacket, pkt);
	END_BROADCAST(world->players)
	return 0;
}

struct world* world_new(size_t chl_count) {
	struct world* world = xmalloc(sizeof(struct world));
	memset(world, 0, sizeof(struct world));
	world->regions = new_hashmap(1, 1);
	world->entities = new_hashmap(1, 0);
	world->players = new_hashmap(1, 1);
	world->chunks = new_hashmap(1, 1);
	world->chl_count = chl_count;
	world->skylightSubtracted = 0;
	world->scheduledTicks = new_hashmap(1, 0);
	world->tps = 0;
	world->ticksInSecond = 0;
	world->seed = 9876543;
	perlin_init(&world->perlin, world->seed);
	return world;
}

int world_load(struct world* world, char* path) {
	char lp[PATH_MAX];
	snprintf(lp, PATH_MAX, "%s/level.dat", path); // could have a double slash, but its okay
	int fd = open(lp, O_RDONLY);
	if (fd < 0) return -1;
	unsigned char* ld = xmalloc(1024);
	size_t ldc = 1024;
	size_t ldi = 0;
	ssize_t i = 0;
	while ((i = read(fd, ld + ldi, ldc - ldi)) > 0) {
		if (ldc - (ldi += i) < 512) {
			ldc += 1024;
			ld = xrealloc(ld, ldc);
		}
	}
	close(fd);
	if (i < 0) {
		xfree(ld);
		return -1;
	}
	unsigned char* nld = NULL;
	ssize_t ds = nbt_decompress(ld, ldi, (void**) &nld);
	xfree(ld);
	if (ds < 0) {
		return -1;
	}
	if (nbt_read(&world->level, nld, ds) < 0) return -1;
	xfree(nld);
	struct nbt_tag* data = nbt_get(world->level, "Data");
	world->levelType = nbt_get(data, "generatorName")->data.nbt_string;
	world->spawnpos.x = nbt_get(data, "SpawnX")->data.nbt_int;
	world->spawnpos.y = nbt_get(data, "SpawnY")->data.nbt_int;
	world->spawnpos.z = nbt_get(data, "SpawnZ")->data.nbt_int;
	world->time = nbt_get(data, "DayTime")->data.nbt_long;
	world->age = nbt_get(data, "Time")->data.nbt_long;
	world->world_folder = xstrdup(path, 0);
	printf("spawn: %i, %i, %i\n", world->spawnpos.x, world->spawnpos.y, world->spawnpos.z);
	snprintf(lp, PATH_MAX, "%s/region/", path);
	DIR* dir = opendir(lp);
	if (dir != NULL) {
		struct dirent* de = NULL;
		while ((de = readdir(dir)) != NULL) {
			if (!endsWith_nocase(de->d_name, ".mca")) continue;
			snprintf(lp, PATH_MAX, "%s/region/%s", path, de->d_name);
			char* xs = strstr(lp, "/r.") + 3;
			char* zs = strchr(xs, '.') + 1;
			if (zs == NULL) continue;
			struct region* reg = region_new(lp, strtol(xs, NULL, 10), strtol(zs, NULL, 10), world->chl_count);
			uint64_t ri = (((uint64_t)(reg->x) & 0xFFFF) << 16) | (((uint64_t) reg->z) & 0xFFFF);
			put_hashmap(world->regions, ri, reg);
		}
		closedir(dir);
	}
	BEGIN_HASHMAP_ITERATION (plugins)
	struct plugin* plugin = value;
	if (plugin->onWorldLoad != NULL) (*plugin->onWorldLoad)(world);
	END_HASHMAP_ITERATION (plugins)
	return 0;
}

void world_pretick(struct world* world) {
	if (++world->time >= 24000) world->time = 0;
	world->age++;
	float pday = ((float) world->time / 24000.) - .25;
	if (pday < 0.) pday++;
	if (pday > 1.) pday--;
	float cel_angle = 1. - ((cosf(pday * M_PI) + 1.) / 2.);
	cel_angle = pday + (cel_angle - pday) / 3.;
	float psubs = 1. - (cosf(cel_angle * M_PI * 2.) * 2. + .5);
	if (psubs < 0.) psubs = 0.;
	if (psubs > 1.) psubs = 1.;
//TODO: rain, thunder
	world->skylightSubtracted = (uint8_t)(psubs * 11.);
}

void world_tick(struct world* world) {
	int32_t lcg = rand();
	while (1) {
		pthread_mutex_lock (&glob_tick_mut);
		pthread_cond_wait(&glob_tick_cond, &glob_tick_mut);
		pthread_mutex_unlock(&glob_tick_mut);
		beginProfilerSection("world_tick");
		if (tick_counter % 20 == 0) {
			world->tps = world->ticksInSecond;
			world->ticksInSecond = 0;
		}
		world->ticksInSecond++;
		world_pretick(world);
		beginProfilerSection("player_receive_packet");
		BEGIN_HASHMAP_ITERATION(world->players)
		struct player* player = (struct player*) value;
		if (player->incomingPacket->size == 0) continue;
		pthread_mutex_lock(&player->incomingPacket->data_mutex);
		struct packet* wp = pop_nowait_queue(player->incomingPacket);
		while (wp != NULL) {
			player_receive_packet(player, wp);
			freePacket(STATE_PLAY, 0, wp);
			xfree(wp);
			wp = pop_nowait_queue(player->incomingPacket);
		}
		pthread_mutex_unlock(&player->incomingPacket->data_mutex);
		END_HASHMAP_ITERATION(world->players)
		endProfilerSection("player_receive_packet");
		beginProfilerSection("tick_player");
		BEGIN_HASHMAP_ITERATION(world->players)
		struct player* player = (struct player*) value;
		player_tick(world, player);
		tick_entity(world, player->entity); // might need to be moved into separate loop later
		END_HASHMAP_ITERATION(world->players)
		endProfilerSection("tick_player");
		beginProfilerSection("tick_entity");
		BEGIN_HASHMAP_ITERATION(world->entities)
		struct entity* entity = (struct entity*) value;
		if (entity->type != ENT_PLAYER) tick_entity(world, entity);
		END_HASHMAP_ITERATION(world->entities)
		endProfilerSection("tick_entity");
		beginProfilerSection("tick_chunks");
		BEGIN_HASHMAP_ITERATION(world->chunks)
		struct chunk* chunk = (struct chunk*) value;
		beginProfilerSection("tick_chunk_tileentity");
		for (size_t x = 0; x < chunk->tileEntitiesTickable->size; x++) {
			struct tile_entity* te = (struct tile_entity*) chunk->tileEntitiesTickable->data[x];
			if (te == NULL) continue;
			(*te->tick)(world, te);
		}
		endProfilerSection("tick_chunk_tileentity");
		beginProfilerSection("tick_chunk_randomticks");
		if (RANDOM_TICK_SPEED > 0) for (int t = 0; t < 16; t++) {
			struct chunk_section* cs = chunk->sections[t];
			if (cs != NULL) {
				for (int z = 0; z < RANDOM_TICK_SPEED; z++) {
					lcg = lcg * 3 + 1013904223;
					int32_t ctotal = lcg >> 2;
					uint8_t x = ctotal & 0x0f;
					uint8_t z = (ctotal >> 8) & 0x0f;
					uint8_t y = (ctotal >> 16) & 0x0f;
					block b = chunk_get_block(chunk, x, y + (t << 4), z);
					struct block_info* bi = getBlockInfo(b);
					if (bi != NULL && bi->randomTick != NULL) (*bi->randomTick)(world, chunk, b, x + (chunk->x << 4), y + (t << 4), z + (chunk->z << 4));
				}
			}
		}
		endProfilerSection("tick_chunk_randomticks");
		END_HASHMAP_ITERATION(world->chunks)
		beginProfilerSection("tick_chunk_scheduledticks");
		struct prqueue* pq = prqueue_new(0, 0);
		BEGIN_HASHMAP_ITERATION(world->scheduledTicks)
		struct scheduled_tick* st = value;
		if (--st->ticksLeft <= 0) { //
			prqueue_add(pq, st, st->priority);
			/*block b = world_get_block(world, st->x, st->y, st->z);
			 struct block_info* bi = getBlockInfo(b);
			 int k = 0;
			 if (bi->scheduledTick != NULL) k = (*bi->scheduledTick)(world, b, st->x, st->y, st->z);
			 if (k > 0) {
			 st->ticksLeft = k;
			 } else {
			 struct encpos ep;
			 ep.x = st->x;
			 ep.y = st->y;
			 ep.z = st->z;
			 put_hashmap(world->scheduledTicks, *((uint64_t*) &ep), NULL);
			 xfree(st);
			 }*/
		}
		END_HASHMAP_ITERATION(world->scheduledTicks)
		struct scheduled_tick* st = NULL;
		while ((st = prqueue_pop(pq)) != NULL) {
			//printf("%i: %i, %i, %i, #%f\n", tick_counter, st->x, st->y, st->z, st->priority);
			block b = world_get_block(world, st->x, st->y, st->z);
			if (st->src != b) {
				struct encpos ep;
				ep.x = st->x;
				ep.y = st->y;
				ep.z = st->z;
				put_hashmap(world->scheduledTicks, *((uint64_t*) &ep), NULL);
				xfree(st);
				continue;
			}
			struct block_info* bi = getBlockInfo(b);
			int k = 0;
			if (bi->scheduledTick != NULL) k = (*bi->scheduledTick)(world, b, st->x, st->y, st->z);
			if (k > 0) {
				st->ticksLeft = k;
				st->src = world_get_block(world, st->x, st->y, st->z);
			} else if (k < 0) {
				xfree(st);
			} else {
				struct encpos ep;
				ep.x = st->x;
				ep.y = st->y;
				ep.z = st->z;
				put_hashmap(world->scheduledTicks, *((uint64_t*) &ep), NULL);
				xfree(st);
			}
		}
		prqueue_del(pq);
		endProfilerSection("tick_chunk_scheduledticks");
		endProfilerSection("tick_chunks");
		BEGIN_HASHMAP_ITERATION (plugins)
		struct plugin* plugin = value;
		if (plugin->tick_world != NULL) (*plugin->tick_world)(world);
		END_HASHMAP_ITERATION (plugins)
		endProfilerSection("world_tick");
	}
}

int world_save(struct world* world, char* path) {

	return 0;
}

void world_free(struct world* world) { // assumes all chunks are unloaded
	BEGIN_HASHMAP_ITERATION(world->regions)
	region_free(value);
	END_HASHMAP_ITERATION(world->regions)
//pthread_rwlock_destroy(&world->chl);
	del_hashmap(world->regions);
	del_hashmap(world->entities);
	del_hashmap(world->chunks);
	BEGIN_HASHMAP_ITERATION(world->players)
	player_free(value);
	END_HASHMAP_ITERATION(world->players)
	del_hashmap(world->players);
	BEGIN_HASHMAP_ITERATION(world->scheduledTicks)
	xfree(value);
	END_HASHMAP_ITERATION(world->scheduledTicks)
	del_hashmap(world->scheduledTicks);
	if (world->level != NULL) {
		freeNBT(world->level);
		xfree(world->level);
	}
	if (world->world_folder != NULL) xfree(world->world_folder);
	xfree(world);
}

struct chunk* chunk_get_entity_chunk(struct entity* entity) {
	return world_get_chunk(entity->world, ((int32_t) entity->x) >> 4, ((int32_t) entity->z) >> 4);
}

void world_spawn_entity(struct world* world, struct entity* entity) {
	entity->world = world;
	if (entity->loadingPlayers == NULL) {
		entity->loadingPlayers = new_hashmap(1, 1);
	}
	if (entity->attackers == NULL) entity->attackers = new_hashmap(1, 0);
	put_hashmap(world->entities, entity->id, entity);
	struct entity_info* ei = getEntityInfo(entity->type);
	if (ei != NULL) {
		if (ei->initAI != NULL) {
			entity->ai = xcalloc(sizeof(struct aicontext));
			entity->ai->tasks = new_hashmap(1, 0);
			(*ei->initAI)(world, entity);
		}
		if (ei->onSpawned != NULL) (*ei->onSpawned)(world, entity);
	}
//struct chunk* ch = chunk_get_entity_chunk(entity);
//if (ch != NULL) {
//	put_hashmap(ch->entities, entity->id, entity);
//}
}

void world_spawn_player(struct world* world, struct player* player) {
	player->world = world;
	if (player->loadedEntities == NULL) player->loadedEntities = new_hashmap(1, 0);
	if (player->loadedChunks == NULL) player->loadedChunks = new_hashmap(1, 1);
	put_hashmap(world->players, player->entity->id, player);
	se: ;
	world_spawn_entity(world, player->entity);
	BEGIN_HASHMAP_ITERATION (plugins)
	struct plugin* plugin = value;
	if (plugin->onPlayerSpawn != NULL) (*plugin->onPlayerSpawn)(world, player);
	END_HASHMAP_ITERATION (plugins)
}

void world_despawn_player(struct world* world, struct player* player) {
	if (player->openInv != NULL) player_closeWindow(player, player->openInv->windowID);
	world_despawn_entity(world, player->entity);
	BEGIN_HASHMAP_ITERATION(player->loadedEntities)
	if (value == NULL || value == player->entity) continue;
	struct entity* ent = (struct entity*) value;
	put_hashmap(ent->loadingPlayers, player->entity->id, NULL);
	END_HASHMAP_ITERATION(player->loadedEntities)
	del_hashmap(player->loadedEntities);
	player->loadedEntities = NULL;
	BEGIN_HASHMAP_ITERATION(player->loadedChunks)
	struct chunk* pl = (struct chunk*) value;
	if (--pl->playersLoaded <= 0) {
		world_unload_chunk(world, pl);
	}
	END_HASHMAP_ITERATION(player->loadedChunks)
	del_hashmap(player->loadedChunks);
	player->loadedChunks = NULL;
	put_hashmap(world->players, player->entity->id, NULL);
}

void world_despawn_entity(struct world* world, struct entity* entity) {
//struct chunk* ch = chunk_get_entity_chunk(entity);
	put_hashmap(world->entities, entity->id, NULL);
//put_hashmap(ch->entities, entity->id, NULL);
	BEGIN_BROADCAST(entity->loadingPlayers)
	struct packet* pkt = xmalloc(sizeof(struct packet));
	pkt->id = PKT_PLAY_CLIENT_DESTROYENTITIES;
	pkt->data.play_client.destroyentities.count = 1;
	pkt->data.play_client.destroyentities.entity_ids = xmalloc(sizeof(int32_t));
	pkt->data.play_client.destroyentities.entity_ids[0] = entity->id;
	add_queue(bc_player->outgoingPacket, pkt);
	put_hashmap(bc_player->loadedEntities, entity->id, NULL);
	END_BROADCAST(entity->loadingPlayers)
	del_hashmap(entity->loadingPlayers);
	entity->loadingPlayers = NULL;
	BEGIN_HASHMAP_ITERATION(entity->attackers)
	struct entity* attacker = value;
	if (attacker->attacking == entity) attacker->attacking = NULL;
	END_HASHMAP_ITERATION(entity->attackers)
	del_hashmap(entity->attackers);
	entity->attackers = NULL;
	entity->attacking = NULL;
}

struct entity* world_get_entity(struct world* world, int32_t id) {
	return get_hashmap(world->entities, id);
}

void world_update_block_guess(struct world* world, struct chunk* chunk, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return;
	block b = world_get_block_guess(world, chunk, x, y, z);
	struct block_info* bi = getBlockInfo(b);
	if (bi != NULL && bi->onBlockUpdate != NULL) bi->onBlockUpdate(world, b, x, y, z);
}

void world_update_block(struct world* world, int32_t x, int32_t y, int32_t z) {
	if (y < 0 || y > 255) return;
	block b = world_get_block(world, x, y, z);
	struct block_info* bi = getBlockInfo(b);
	if (bi != NULL && bi->onBlockUpdate != NULL) bi->onBlockUpdate(world, b, x, y, z);
}
