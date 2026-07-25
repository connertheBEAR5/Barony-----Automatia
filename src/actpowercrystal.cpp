/*-------------------------------------------------------------------------------

BARONY
File: actpowercrystal.cpp
Desc: behavior function for power crystals

Copyright 2013-2016 (c) Turning Wheel LLC, all rights reserved.
See LICENSE for details.

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "monster.hpp"
#include "engine/audio/sound.hpp"
#include "items.hpp"
#include "net.hpp"
#include "collision.hpp"
#include "player.hpp"
#include "prng.hpp"

/*-------------------------------------------------------------------------------

act*

The following function describes an entity behavior. The function
takes a pointer to the entity that uses it as an argument.

-------------------------------------------------------------------------------*/

void actPowerCrystalBase(Entity* my)
{
	if ( my->flags[PASSABLE] ) // stop the compiler optimising into a different entity.
	{
		my->flags[PASSABLE] = false;
	}

	return;
}

void actPowerCrystal(Entity* my)
{
	if ( !my )
	{
		return;
	}

	my->actPowerCrystal();
}

void Entity::actPowerCrystal()
{
	//Entity* entity;
	real_t upper_z = this->crystalStartZ - 0.4;
	real_t lower_z = crystalStartZ + 0.4;
	int i = 0;

	real_t acceleration = 0.95;

	if ( ticks == 1 )
	{
		this->createWorldUITooltip();
	}

	/*
 * A circuit-powered crystal requires live power.
 *
 * Unlike unlock-spell activation, this is not latched:
 *
 * circuit ON  -> activate and generate output
 * circuit OFF -> deactivate and remove output
 */
const bool requiresCircuitPower =
    crystalPowerToActivate != 0;

/*
 * External activation input is separate from the crystal's own
 * directional output state.
 */
const bool hasCircuitPower =
    crystalExternalPower > 1;

/*
 * Deactivate a circuit-powered crystal when its incoming power is
 * removed.
 *
 * This is host-authoritative. Clients receive the resulting entity
 * state and circuit changes from the server.
 */
if ( multiplayer != CLIENT
    && requiresCircuitPower
    && crystalInitialised
    && !hasCircuitPower )
{
	/*
	* Mark the crystal's output OFF without broadcasting from the
	* crystal's own tile.
	*/
	this->skill[28] = CIRCUIT_OFF;

	if ( multiplayer == SERVER )
	{
		serverUpdateEntitySkill(
			this,
			28
		);
	}

	node_t* node = nullptr;
	node_t* nextnode = nullptr;

    for ( node = this->children.first;
        node != nullptr;
        node = nextnode )
    {
        nextnode = node->next;

        if ( node->element != nullptr )
        {
			Entity* electricityNode =
				static_cast<Entity*>(node->element);

			/*
			* Depower the generated output before deleting it. This sends OFF
			* through only the crystal's directional output network.
			*/
			if ( electricityNode->behavior == &actCircuit )
			{
				electricityNode->circuitPowerOff();
			}

			if ( electricityNode->light != nullptr )
            {
                list_RemoveNode(
                    electricityNode->light->node
                );

                electricityNode->light = nullptr;
            }

            if ( electricityNode->mynode != nullptr )
            {
                list_RemoveNode(
                    electricityNode->mynode
                );
            }
        }

        list_RemoveNode(node);
    }

    crystalGeneratedElectricityNodes = 0;
    crystalInitialised = 0;
    crystalTurning = 0;

    /*
     * Return to the lowered inactive position.
     */
    this->z =
        crystalStartZ + 5;

    this->new_z =
        this->z;

    this->vel_z =
        crystalMaxZVelocity * 2;

    this->bNeedsRenderPositionInit = true;
    this->flags[UPDATENEEDED] = true;

    serverUpdateEntitySkill(this, 1);
    serverUpdateEntitySkill(this, 3);
    serverUpdateEntitySkill(this, 5);
}

/*
 * Normal crystals activate immediately.
 *
 * Unlock-spell crystals continue using their existing spell
 * activation behavior.
 *
 * Circuit-powered crystals activate only while receiving power.
 */
const bool crystalMayActivate =
    !crystalSpellToActivate
    && (
        !requiresCircuitPower
        || hasCircuitPower
    );

/*
 * Electrically activate immediately when external power arrives.
 *
 * The crystal's vertical movement is visual only and must not delay
 * the circuit output.
 */
if ( !crystalInitialised
    && crystalMayActivate )
{
    crystalInitialised = 1;
    crystalTurning = 0;

    /*
     * Create and power the directional nodes immediately on the same
     * lever state change.
     */
    this->powerCrystalCreateElectricityNodes();

    if ( multiplayer == SERVER )
    {
        serverUpdateEntitySkill(
            this,
            1
        );

        serverUpdateEntitySkill(
            this,
            3
        );
    }
}

/*
 * After activation, visually raise the crystal toward its normal
 * hovering position.
 */
if ( crystalInitialised
    && this->z > crystalStartZ )
{
    this->z -=
        this->vel_z
        * (1 / acceleration);

    if ( this->z <= crystalStartZ )
    {
        this->z =
            crystalStartZ;

        this->vel_z =
            crystalMaxZVelocity;
    }

    this->new_z =
        this->z;
}
/*
 * Do not begin normal hovering until the crystal has reached its
 * normal center position.
 */
if ( crystalInitialised
    && this->z <= crystalStartZ )
{
		if ( crystalHoverDirection == CRYSTAL_HOVER_UP ) //rise state
		{
			this->z -= this->vel_z;

			if ( this->z < upper_z )
			{
				this->z = upper_z;
				crystalHoverDirection = CRYSTAL_HOVER_UP_WAIT;
			}

			if ( this->z < crystalStartZ ) //higher than mid point
			{
				this->vel_z = std::max(this->vel_z * acceleration, crystalMinZVelocity);
			}
			else if ( this->z > crystalStartZ ) //lower than midpoint
			{
				this->vel_z = std::min(this->vel_z * (1 / acceleration), crystalMaxZVelocity);
			}
		}
		else if ( crystalHoverDirection == CRYSTAL_HOVER_UP_WAIT ) // wait state
		{
			crystalHoverWaitTimer++;
			if ( crystalHoverWaitTimer >= 1 )
			{
				crystalHoverDirection = CRYSTAL_HOVER_DOWN; // advance state
				crystalHoverWaitTimer = 0; // reset timer
			}
		}
		else if ( crystalHoverDirection == CRYSTAL_HOVER_DOWN ) //fall state
		{
			this->z += this->vel_z;

			if ( this->z > lower_z )
			{
				this->z = lower_z;
				crystalHoverDirection = CRYSTAL_HOVER_DOWN_WAIT;
			}

			if ( this->z < crystalStartZ ) //higher than mid point, start accelerating
			{
				this->vel_z = std::min(this->vel_z * (1 / acceleration), crystalMaxZVelocity);
			}
			else if ( this->z > crystalStartZ ) //lower than midpoint, start decelerating
			{
				this->vel_z = std::max(this->vel_z * acceleration, crystalMinZVelocity);
			}
		}
		else if ( crystalHoverDirection == CRYSTAL_HOVER_DOWN_WAIT ) // wait state
		{
			crystalHoverWaitTimer++;
			if ( crystalHoverWaitTimer >= 1 )
			{
				crystalHoverDirection = CRYSTAL_HOVER_UP; // advance state
				crystalHoverWaitTimer = 0; // reset timer
			}
		}


		if ( this->z <= crystalStartZ + crystalMaxZVelocity && this->z >= crystalStartZ - crystalMaxZVelocity )
		{
			this->vel_z = this->fskill[1]; // reset velocity at the mid point of animation
		}

		spawnAmbientParticles(80, 579, 10 + local_rng.rand() % 40, 1.0, false);

		if ( crystalTurning == 1 )
		{
			if ( !crystalTurnReverse )
			{
				this->yaw += crystalTurnVelocity; // reverse velocity if turnReverse is 1

				if ( (this->yaw >= (crystalTurnStartDir * (PI / 2)) + (PI / 2)) )
				{
					this->yaw = crystalTurnStartDir * (PI / 2) + (PI / 2);
					crystalTurning = 0;

					if ( this->yaw >= 2 * PI )
					{
						this->yaw = 0;
					}
					this->powerCrystalCreateElectricityNodes();
				}
			}
			else
			{
				this->yaw -= crystalTurnVelocity;// reverse velocity if turnReverse is 1

				if ( (this->yaw <= (crystalTurnStartDir * (PI / 2)) - (PI / 2)) )
				{
					this->yaw = crystalTurnStartDir * (PI / 2) - (PI / 2);
					crystalTurning = 0;

					if ( this->yaw < 0 )
					{
						this->yaw += 2 * PI;
					}
					this->powerCrystalCreateElectricityNodes();
				}
			}
		}
	}

	if ( multiplayer == CLIENT )
	{
		return;
	}

	// handle player turning the crystal

	for ( i = 0; i < MAXPLAYERS; i++ )
	{
		if ( (client_selected[i] == this || selectedEntity[i] == this) && crystalTurning == 0 )
		{
			if ( inrange[i] )
			{
				if ( players[i] && Player::getPlayerInteractEntity(i) && crystalInitialised )
				{
					playSoundEntity(this, 151, 128);
					crystalTurning = 1;
					crystalTurnStartDir = static_cast<Sint32>(this->yaw / (PI / 2));
					serverUpdateEntitySkill(this, 3);
					serverUpdateEntitySkill(this, 4);
					messagePlayer(i, MESSAGE_INTERACTION, Language::get(2356));
				}
				else if ( !crystalInitialised )
				{
					messagePlayer(i, MESSAGE_INTERACTION, Language::get(2357));
				}
			}	
		}
	}

	return;
}

// ambient particle effects.
void actPowerCrystalParticleIdle(Entity* my)
{
	if ( !my )
	{
		return;
	}

	if ( my->skill[0] < 0 )
	{
		list_RemoveNode(my->mynode);
		return;
	}
	else
	{
		--my->skill[0];
		my->z += my->vel_z;
		//my->z -= 0.01;
	}
	return;
}

void Entity::powerCrystalCreateElectricityNodes()
{
	Entity* entity = nullptr;
	node_t* node = nullptr;
	node_t* nextnode = nullptr;
	real_t xtest = 0;
	real_t ytest = 0;
	
	int i = 0;

if ( crystalGeneratedElectricityNodes )
{
    this->skill[28] =
        CIRCUIT_OFF;

    if ( multiplayer == SERVER )
    {
        serverUpdateEntitySkill(
            this,
            28
        );
    }

    if ( multiplayer != CLIENT )
    {
        for ( node = this->children.first;
            node != nullptr;
            node = nextnode )
        {
            nextnode = node->next;

            if ( node->element != nullptr )
            {
                entity =
                    static_cast<Entity*>(node->element);

                if ( entity->behavior == &actCircuit )
                {
                    entity->circuitPowerOff();
                }

                if ( entity->light != nullptr )
                {
                    list_RemoveNode(
                        entity->light->node
                    );
                }

                entity->light = nullptr;

                if ( entity->mynode != nullptr )
                {
                    list_RemoveNode(
                        entity->mynode
                    );
                }
            }

            list_RemoveNode(node);
        }
    }
}

	for ( i = 1; i <= crystalNumElectricityNodes; i++ )
	{
		entity = newEntity(-1, 0, map.entities, nullptr); // electricity node
		/*
		* Identify this as an output node belonging to this crystal.
		* This prevents the generated output from feeding back into the
		* crystal's external activation input.
		*/
		entity->parent = this->uid;
		xtest = this->x + i * 16 * ((this->yaw == 0) - (this->yaw == PI)); // add/subtract x depending on direction.
		ytest = this->y + i * 16 * ((this->yaw == PI / 2) - (this->yaw == 3 * PI / 2)); // add/subtract y depending on direction.
		
		if ( (static_cast<int>(xtest) >> 4) < 0 || (static_cast<int>(xtest) >> 4) >= map.width || 
			(static_cast<int>(ytest) >> 4) < 0 || (static_cast<int>(ytest) >> 4) >= map.height )
		{
			//messagePlayer(0, "stopped at index %d, x: %d, y: %d", i, (static_cast<int>(xtest) >> 4), (static_cast<int>(ytest) >> 4));
			break; // stop generating more nodes as we are out of bounds
		}
		
		//messagePlayer(0, "gen at index %d", i);
		entity->x = xtest;
		entity->y = ytest;
		entity->z = 5;
		entity->behavior = &actCircuit;
		entity->flags[PASSABLE] = true;
		entity->flags[INVISIBLE] = true;
		entity->flags[NOUPDATE] = true;
		entity->circuit_status = CIRCUIT_OFF; //It's a depowered powerable.

		node = list_AddNodeLast(&this->children);
		node->element = entity; // add the node to the children list.
		node->deconstructor = &emptyDeconstructor;
		node->size = sizeof(Entity*);

		TileEntityList.addEntity(*entity); // make sure new nodes are added to the tile list to properly update neighbors.

		this->crystalGeneratedElectricityNodes = 1;
	}
	
/*
 * Record that the crystal's directional output is ON, but do not
 * broadcast power from the crystal entity itself.
 *
 * Broadcasting from the crystal would also power the external input
 * wire beside it, causing the circuit to latch itself.
 */
this->skill[28] = CIRCUIT_ON;

/*
 * Power begins at the generated directional circuit nodes.
 */
if ( multiplayer != CLIENT )
{
    for ( node = this->children.first;
        node != nullptr;
        node = node->next )
    {
        if ( !node->element )
        {
            continue;
        }

        Entity* electricityNode =
            static_cast<Entity*>(node->element);

        if ( electricityNode->behavior == &actCircuit )
        {
            electricityNode->circuitPowerOn();
        }
    }
}

if ( multiplayer == SERVER )
{
    serverUpdateEntitySkill(
        this,
        28
    );
}

return;
}
