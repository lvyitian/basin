/*
 * player.c
 *
 *  Created on: Jun 24, 2016
 *      Author: root
 */

#include "network.h"
#include "entity.h"
#include "accept.h"
#include "util.h"
#include "inventory.h"
#include "xstring.h"
#include "queue.h"
#include "packet.h"
#include "game.h"
#include "collection.h"
#include "player.h"
#include "globals.h"
#include "item.h"
#include "tileentity.h"
#include <math.h>
#include "block.h"
#include "world.h"

struct player* newPlayer(struct entity* entity, char* name, struct uuid uuid, struct conn* conn, uint8_t gamemode) {
	struct player* player = xmalloc(sizeof(struct player));
	entity->data.player.player = player;
	player->entity = entity;
	player->conn = conn;
	player->name = name;
	player->uuid = uuid;
	player->gamemode = gamemode;
	player->currentItem = 0;
	player->ping = 0;
	player->stage = 0;
	player->invulnerable = 0;
	player->mayfly = 0;
	player->instabuild = 0;
	player->walkSpeed = 0;
	player->flySpeed = 0;
	player->maybuild = 0;
	player->flying = 0;
	player->xpseed = 0;
	player->xptotal = 0;
	player->xplevel = 0;
	player->score = 0;
	player->saturation = 0.;
	player->sleeping = 0;
	player->fire = 0;
	//TODO: enderitems & inventory
	player->food = 20;
	player->foodexhaustion = 0.;
	player->foodTick = 0;
	player->nextKeepAlive = 0;
	player->inHand = NULL;
	memset(&player->digging_position, 0, sizeof(struct encpos));
	player->digging = -1.;
	player->digspeed = 0.;
	player->inventory = xmalloc(sizeof(struct inventory));
	newInventory(player->inventory, INVTYPE_PLAYERINVENTORY, 0, 46);
	player->openInv = NULL;
	put_hashmap(player->inventory->players, player->entity->id, player);
	player->loadedChunks = new_hashmap(1, 1);
	player->loadedEntities = new_hashmap(1, 0);
	player->incomingPacket = new_queue(0, 1);
	player->outgoingPacket = new_queue(0, 1);
	player->defunct = 0;
	player->lastSwing = tick_counter;
	player->foodTimer = 0;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	player->reachDistance = 6.f;
	player->acstate.real_onGround = 1;
	player->acstate.flightInfraction = 0;
	player->acstate.ldy = 0.;
	player->acstate.lastJump = 0;
	player->acstate.offGroundTime = 0;
	player->spawnedIn = 0;
	player->llTick = 0;
	player->triggerRechunk = 0;
	player->chunksSent = 0;
	return player;
}

int player_onGround(struct player* player) {
	struct entity* entity = player->entity;
	struct boundingbox obb;
	getEntityCollision(entity, &obb);
	if (obb.minX == obb.maxX || obb.minZ == obb.maxZ || obb.minY == obb.maxY) {
		return 0;
	}
	obb.minY += -.08;
	struct boundingbox pbb;
	getEntityCollision(entity, &pbb);
	double ny = -.08;
	for (int32_t x = floor(obb.minX); x < floor(obb.maxX + 1.); x++) {
		for (int32_t z = floor(obb.minZ); z < floor(obb.maxZ + 1.); z++) {
			for (int32_t y = floor(obb.minY); y < floor(obb.maxY + 1.); y++) {
				block b = getBlockWorld(entity->world, x, y, z);
				if (b == 0) continue;
				struct block_info* bi = getBlockInfo(b);
				if (bi == NULL) continue;
				for (size_t i = 0; i < bi->boundingBox_count; i++) {
					struct boundingbox* bb = &bi->boundingBoxes[i];
					if (b > 0 && bb->minX != bb->maxX && bb->minY != bb->maxY && bb->minZ != bb->maxZ) {
						if (bb->maxX + x > obb.minX && bb->minX + x < obb.maxX ? (bb->maxY + y > obb.minY && bb->minY + y < obb.maxY ? bb->maxZ + z > obb.minZ && bb->minZ + z < obb.maxZ : 0) : 0) {
							if (pbb.maxX > bb->minX + x && pbb.minX < bb->maxX + x && pbb.maxZ > bb->minZ + z && pbb.minZ < bb->maxZ + z) {
								double t;
								if (ny > 0. && pbb.maxY <= bb->minY + y) {
									t = bb->minY + y - pbb.maxY;
									if (t < ny) {
										ny = t;
									}
								} else if (ny < 0. && pbb.minY >= bb->maxY + y) {
									t = bb->maxY + y - pbb.minY;
									if (t > ny) {
										ny = t;
									}
								}
							}
						}
					}
				}
			}
		}
	}
	return fabs(-.08 - ny) > .001;
}

void kickPlayer(struct player* player, char* message) {
	struct packet* pkt = xmalloc(sizeof(struct packet));
	pkt->id = PKT_PLAY_CLIENT_DISCONNECT;
	size_t sl = strlen(message);
	pkt->data.play_client.disconnect.reason = xmalloc(sl + 512);
	snprintf(pkt->data.play_client.disconnect.reason, sl + 512, "{\"text\": \"%s\", \"color\": \"red\"}", message);
	add_queue(player->outgoingPacket, pkt);
	if (player->conn != NULL) player->conn->disconnect = 1;
	broadcastf("red", "Kicked Player %s for reason: %s", player->name, message);
}

float player_getAttackStrength(struct player* player, float adjust) {
	float cooldownPeriod = 4.;
	struct slot* sl = getSlot(player, player->inventory, 36 + player->currentItem);
	if (sl != NULL) {
		struct item_info* ii = getItemInfo(sl->item);
		if (ii != NULL) {
			cooldownPeriod = ii->attackSpeed;
		}
	}
	float str = ((float) (tick_counter - player->lastSwing) + adjust) * cooldownPeriod / 20.;
	if (str > 1.) str = 1.;
	if (str < 0.) str = 0.;
	return str;
}

void teleportPlayer(struct player* player, double x, double y, double z) {
	player->entity->x = x;
	player->entity->y = y;
	player->entity->z = z;
	player->entity->lx = x;
	player->entity->ly = y;
	player->entity->lz = z;
	player->triggerRechunk = 1;
	player->spawnedIn = 0;
	struct packet* pkt = xmalloc(sizeof(struct packet));
	pkt->id = PKT_PLAY_CLIENT_ENTITYVELOCITY;
	pkt->data.play_client.entityvelocity.entity_id = player->entity->id;
	pkt->data.play_client.entityvelocity.velocity_x = 0;
	pkt->data.play_client.entityvelocity.velocity_y = 0;
	pkt->data.play_client.entityvelocity.velocity_z = 0;
	add_queue(player->outgoingPacket, pkt);
	pkt = xmalloc(sizeof(struct packet));
	pkt->id = PKT_PLAY_CLIENT_PLAYERPOSITIONANDLOOK;
	pkt->data.play_client.playerpositionandlook.x = player->entity->x;
	pkt->data.play_client.playerpositionandlook.y = player->entity->y;
	pkt->data.play_client.playerpositionandlook.z = player->entity->z;
	pkt->data.play_client.playerpositionandlook.yaw = player->entity->yaw;
	pkt->data.play_client.playerpositionandlook.pitch = player->entity->pitch;
	pkt->data.play_client.playerpositionandlook.flags = 0x0;
	pkt->data.play_client.playerpositionandlook.teleport_id = 0;
	add_queue(player->outgoingPacket, pkt);
//	if (player->tps > 0) player->tps--;
}

struct player* getPlayerByName(char* name) {
	BEGIN_HASHMAP_ITERATION (players)
	struct player* player = (struct player*) value;
	if (player != NULL && streq_nocase(name, player->name)) {
		BREAK_HASHMAP_ITERATION(players);
		return player;
	}
	END_HASHMAP_ITERATION (players)
	return NULL;
}

void player_closeWindow(struct player* player, uint16_t windowID) {
	struct inventory* inv = NULL;
	if (windowID == 0 && player->openInv == NULL) inv = player->inventory;
	else if (player->openInv != NULL && windowID == player->openInv->windowID) inv = player->openInv;
	if (inv != NULL) {
		if (player->inHand != NULL) {
			dropPlayerItem(player, player->inHand);
			freeSlot(player->inHand);
			xfree(player->inHand);
			player->inHand = NULL;
		}
		if (inv->type == INVTYPE_PLAYERINVENTORY) {
			for (int i = 1; i < 5; i++) {
				if (inv->slots[i] != NULL) {
					dropPlayerItem(player, inv->slots[i]);
					setSlot(player, inv, i, NULL, 0, 1);
				}
			}
		} else if (inv->type == INVTYPE_WORKBENCH) {
			for (int i = 1; i < 10; i++) {
				if (inv->slots[i] != NULL) {
					dropPlayerItem(player, inv->slots[i]);
					setSlot(player, inv, i, NULL, 0, 1);
				}
			}
			freeInventory(inv);
			inv = NULL;
		} else if (inv->type == INVTYPE_CHEST) {
			if (inv->te != NULL) {
				BEGIN_BROADCAST_DIST(player->entity, 128.)
				struct packet* pkt = xmalloc(sizeof(struct packet));
				pkt->id = PKT_PLAY_CLIENT_BLOCKACTION;
				pkt->data.play_client.blockaction.location.x = inv->te->x;
				pkt->data.play_client.blockaction.location.y = inv->te->y;
				pkt->data.play_client.blockaction.location.z = inv->te->z;
				pkt->data.play_client.blockaction.action_id = 1;
				pkt->data.play_client.blockaction.action_param = inv->players->entry_count - 1;
				pkt->data.play_client.blockaction.block_type = getBlockWorld(player->world, inv->te->x, inv->te->y, inv->te->z) >> 4;
				add_queue(bc_player->outgoingPacket, pkt);
				END_BROADCAST(player->world->players)
			}
		}
	}
	if (inv != NULL && inv->type != INVTYPE_PLAYERINVENTORY) put_hashmap(inv->players, player->entity->id, NULL);
	player->openInv = NULL; // TODO: free sometimes?

}

void setPlayerGamemode(struct player* player, int gamemode) {
	if (gamemode != -1) {
		player->gamemode = gamemode;
		struct packet* pkt = xmalloc(sizeof(struct packet));
		pkt->id = PKT_PLAY_CLIENT_CHANGEGAMESTATE;
		pkt->data.play_client.changegamestate.reason = 3;
		pkt->data.play_client.changegamestate.value = gamemode;
		add_queue(player->outgoingPacket, pkt);
	}
	struct packet* pkt = xmalloc(sizeof(struct packet));
	pkt->id = PKT_PLAY_CLIENT_PLAYERABILITIES;
	pkt->data.play_client.playerabilities.flags = player->gamemode == 1 ? (0x04 | 0x08) : 0x0;
	pkt->data.play_client.playerabilities.flying_speed = .05;
	pkt->data.play_client.playerabilities.field_of_view_modifier = .1;
	add_queue(player->outgoingPacket, pkt);
}

void freePlayer(struct player* player) {
	struct packet* pkt = NULL;
	while ((pkt = pop_nowait_queue(player->incomingPacket)) != NULL) {
		freePacket(STATE_PLAY, 0, pkt);
		xfree(pkt);
	}
	del_queue(player->incomingPacket);
	while ((pkt = pop_nowait_queue(player->outgoingPacket)) != NULL) {
		freePacket(STATE_PLAY, 1, pkt);
		xfree(pkt);
	}
	del_queue(player->outgoingPacket);
	if (player->inHand != NULL) {
		freeSlot(player->inHand);
		xfree(player->inHand);
	}
	del_hashmap(player->loadedChunks);
	del_hashmap(player->loadedEntities);
	freeInventory(player->inventory);
	xfree(player->inventory);
	xfree(player->name);
	xfree(player);
}
