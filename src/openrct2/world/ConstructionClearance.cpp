/*****************************************************************************
 * Copyright (c) 2014-2021 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ConstructionClearance.h"

#include "../Game.h"
#include "../openrct2/Cheats.h"
#include "../ride/Ride.h"
#include "../ride/RideData.h"
#include "Park.h"
#include "Scenery.h"
#include "SmallScenery.h"
#include "Surface.h"

static int32_t map_place_clear_func(
    TileElement** tile_element, const CoordsXY& coords, uint8_t flags, money32* price, bool is_scenery)
{
    if ((*tile_element)->GetType() != TILE_ELEMENT_TYPE_SMALL_SCENERY)
        return 1;

    if (is_scenery && !(flags & GAME_COMMAND_FLAG_PATH_SCENERY))
        return 1;

    auto* scenery = (*tile_element)->AsSmallScenery()->GetEntry();

    if (gParkFlags & PARK_FLAGS_FORBID_TREE_REMOVAL)
    {
        if (scenery != nullptr && scenery->HasFlag(SMALL_SCENERY_FLAG_IS_TREE))
            return 1;
    }

    if (!(gParkFlags & PARK_FLAGS_NO_MONEY) && scenery != nullptr)
        *price += scenery->removal_price * 10;

    if (flags & GAME_COMMAND_FLAG_GHOST)
        return 0;

    if (!(flags & GAME_COMMAND_FLAG_APPLY))
        return 0;

    map_invalidate_tile({ coords, (*tile_element)->GetBaseZ(), (*tile_element)->GetClearanceZ() });

    tile_element_remove(*tile_element);

    (*tile_element)--;
    return 0;
}

/**
 *
 *  rct2: 0x006E0D6E, 0x006B8D88
 */
int32_t map_place_scenery_clear_func(TileElement** tile_element, const CoordsXY& coords, uint8_t flags, money32* price)
{
    return map_place_clear_func(tile_element, coords, flags, price, /*is_scenery=*/true);
}

/**
 *
 *  rct2: 0x006C5A4F, 0x006CDE57, 0x006A6733, 0x0066637E
 */
int32_t map_place_non_scenery_clear_func(TileElement** tile_element, const CoordsXY& coords, uint8_t flags, money32* price)
{
    return map_place_clear_func(tile_element, coords, flags, price, /*is_scenery=*/false);
}

static bool MapLoc68BABCShouldContinue(
    TileElement* tileElement, const CoordsXYRangedZ& pos, CLEAR_FUNC clearFunc, uint8_t flags, money32& price,
    uint8_t crossingMode, bool canBuildCrossing)
{
    if (clearFunc != nullptr)
    {
        if (!clearFunc(&tileElement, pos, flags, &price))
        {
            return true;
        }
    }

    // Crossing mode 1: building track over path
    if (crossingMode == 1 && canBuildCrossing && tileElement->GetType() == TILE_ELEMENT_TYPE_PATH
        && tileElement->GetBaseZ() == pos.baseZ && !tileElement->AsPath()->IsQueue() && !tileElement->AsPath()->IsSloped())
    {
        return true;
    }
    // Crossing mode 2: building path over track
    else if (
        crossingMode == 2 && canBuildCrossing && tileElement->GetType() == TILE_ELEMENT_TYPE_TRACK
        && tileElement->GetBaseZ() == pos.baseZ && tileElement->AsTrack()->GetTrackType() == TrackElemType::Flat)
    {
        auto ride = get_ride(tileElement->AsTrack()->GetRideIndex());
        if (ride != nullptr && ride->GetRideTypeDescriptor().HasFlag(RIDE_TYPE_FLAG_SUPPORTS_LEVEL_CROSSINGS))
        {
            return true;
        }
    }

    return false;
}

/**
 *
 *  rct2: 0x0068B932
 *  ax = x
 *  cx = y
 *  dl = zLow
 *  dh = zHigh
 *  ebp = clearFunc
 *  bl = bl
 */
GameActions::Result::Ptr MapCanConstructWithClearAt(
    const CoordsXYRangedZ& pos, CLEAR_FUNC clearFunc, QuarterTile quarterTile, uint8_t flags, uint8_t crossingMode, bool isTree)
{
    auto res = std::make_unique<GameActions::Result>();

    uint8_t groundFlags = ELEMENT_IS_ABOVE_GROUND;
    bool canBuildCrossing = false;
    if (map_is_edge(pos))
    {
        res->Error = GameActions::Status::InvalidParameters;
        res->ErrorMessage = STR_OFF_EDGE_OF_MAP;
        return res;
    }

    if (gCheatsDisableClearanceChecks)
    {
        res->SetData(ConstructClearResult{ groundFlags });
        return res;
    }

    TileElement* tileElement = map_get_first_element_at(pos);
    if (tileElement == nullptr)
    {
        res->Error = GameActions::Status::Unknown;
        res->ErrorMessage = STR_NONE;
        return res;
    }

    do
    {
        if (tileElement->GetType() != TILE_ELEMENT_TYPE_SURFACE)
        {
            if (pos.baseZ < tileElement->GetClearanceZ() && pos.clearanceZ > tileElement->GetBaseZ()
                && !(tileElement->IsGhost()))
            {
                if (tileElement->GetOccupiedQuadrants() & (quarterTile.GetBaseQuarterOccupied()))
                {
                    if (MapLoc68BABCShouldContinue(
                            tileElement, pos, clearFunc, flags, res->Cost, crossingMode, canBuildCrossing))
                    {
                        continue;
                    }

                    map_obstruction_set_error_text(tileElement, *res);
                    res->Error = GameActions::Status::NoClearance;
                    return res;
                }
            }
            continue;
        }

        const auto waterHeight = tileElement->AsSurface()->GetWaterHeight();
        if (waterHeight && waterHeight > pos.baseZ && tileElement->GetBaseZ() < pos.clearanceZ)
        {
            groundFlags |= ELEMENT_IS_UNDERWATER;
            if (waterHeight < pos.clearanceZ)
            {
                if (clearFunc != nullptr && clearFunc(&tileElement, pos, flags, &res->Cost))
                {
                    res->Error = GameActions::Status::NoClearance;
                    res->ErrorMessage = STR_CANNOT_BUILD_PARTLY_ABOVE_AND_PARTLY_BELOW_WATER;
                    return res;
                }
            }
        }

        if (gParkFlags & PARK_FLAGS_FORBID_HIGH_CONSTRUCTION && !isTree)
        {
            const auto heightFromGround = pos.clearanceZ - tileElement->GetBaseZ();

            if (heightFromGround > (18 * COORDS_Z_STEP))
            {
                res->Error = GameActions::Status::Disallowed;
                res->ErrorMessage = STR_LOCAL_AUTHORITY_WONT_ALLOW_CONSTRUCTION_ABOVE_TREE_HEIGHT;
                return res;
            }
        }

        // Only allow building crossings directly on a flat surface tile.
        if (tileElement->GetType() == TILE_ELEMENT_TYPE_SURFACE
            && (tileElement->AsSurface()->GetSlope()) == TILE_ELEMENT_SLOPE_FLAT && tileElement->GetBaseZ() == pos.baseZ)
        {
            canBuildCrossing = true;
        }

        if (quarterTile.GetZQuarterOccupied() != 0b1111)
        {
            if (tileElement->GetBaseZ() >= pos.clearanceZ)
            {
                // loc_68BA81
                groundFlags |= ELEMENT_IS_UNDERGROUND;
                groundFlags &= ~ELEMENT_IS_ABOVE_GROUND;
            }
            else
            {
                auto northZ = tileElement->GetBaseZ();
                auto eastZ = northZ;
                auto southZ = northZ;
                auto westZ = northZ;
                const auto slope = tileElement->AsSurface()->GetSlope();
                if (slope & TILE_ELEMENT_SLOPE_N_CORNER_UP)
                {
                    northZ += LAND_HEIGHT_STEP;
                    if (slope == (TILE_ELEMENT_SLOPE_S_CORNER_DN | TILE_ELEMENT_SLOPE_DOUBLE_HEIGHT))
                        northZ += LAND_HEIGHT_STEP;
                }
                if (slope & TILE_ELEMENT_SLOPE_E_CORNER_UP)
                {
                    eastZ += LAND_HEIGHT_STEP;
                    if (slope == (TILE_ELEMENT_SLOPE_W_CORNER_DN | TILE_ELEMENT_SLOPE_DOUBLE_HEIGHT))
                        eastZ += LAND_HEIGHT_STEP;
                }
                if (slope & TILE_ELEMENT_SLOPE_S_CORNER_UP)
                {
                    southZ += LAND_HEIGHT_STEP;
                    if (slope == (TILE_ELEMENT_SLOPE_N_CORNER_DN | TILE_ELEMENT_SLOPE_DOUBLE_HEIGHT))
                        southZ += LAND_HEIGHT_STEP;
                }
                if (slope & TILE_ELEMENT_SLOPE_W_CORNER_UP)
                {
                    westZ += LAND_HEIGHT_STEP;
                    if (slope == (TILE_ELEMENT_SLOPE_E_CORNER_DN | TILE_ELEMENT_SLOPE_DOUBLE_HEIGHT))
                        westZ += LAND_HEIGHT_STEP;
                }
                const auto baseHeight = pos.baseZ + (4 * COORDS_Z_STEP);
                const auto baseQuarter = quarterTile.GetBaseQuarterOccupied();
                const auto zQuarter = quarterTile.GetZQuarterOccupied();
                if ((!(baseQuarter & 0b0001) || ((zQuarter & 0b0001 || pos.baseZ >= northZ) && baseHeight >= northZ))
                    && (!(baseQuarter & 0b0010) || ((zQuarter & 0b0010 || pos.baseZ >= eastZ) && baseHeight >= eastZ))
                    && (!(baseQuarter & 0b0100) || ((zQuarter & 0b0100 || pos.baseZ >= southZ) && baseHeight >= southZ))
                    && (!(baseQuarter & 0b1000) || ((zQuarter & 0b1000 || pos.baseZ >= westZ) && baseHeight >= westZ)))
                {
                    continue;
                }

                if (MapLoc68BABCShouldContinue(tileElement, pos, clearFunc, flags, res->Cost, crossingMode, canBuildCrossing))
                {
                    continue;
                }

                map_obstruction_set_error_text(tileElement, *res);
                res->Error = GameActions::Status::NoClearance;
                return res;
            }
        }
    } while (!(tileElement++)->IsLastForTile());

    res->SetData(ConstructClearResult{ groundFlags });

    return res;
}

GameActions::Result::Ptr MapCanConstructAt(const CoordsXYRangedZ& pos, QuarterTile bl)
{
    return MapCanConstructWithClearAt(pos, nullptr, bl, 0);
}

/**
 *
 *  rct2: 0x0068BB18
 */
void map_obstruction_set_error_text(TileElement* tileElement, GameActions::Result& res)
{
    Ride* ride;

    res.ErrorMessage = STR_OBJECT_IN_THE_WAY;
    switch (tileElement->GetType())
    {
        case TILE_ELEMENT_TYPE_SURFACE:
            res.ErrorMessage = STR_RAISE_OR_LOWER_LAND_FIRST;
            break;
        case TILE_ELEMENT_TYPE_PATH:
            res.ErrorMessage = STR_FOOTPATH_IN_THE_WAY;
            break;
        case TILE_ELEMENT_TYPE_TRACK:
            ride = get_ride(tileElement->AsTrack()->GetRideIndex());
            if (ride != nullptr)
            {
                res.ErrorMessage = STR_X_IN_THE_WAY;

                Formatter ft(res.ErrorMessageArgs.data());
                ride->FormatNameTo(ft);
            }
            break;
        case TILE_ELEMENT_TYPE_SMALL_SCENERY:
        {
            auto* sceneryEntry = tileElement->AsSmallScenery()->GetEntry();
            res.ErrorMessage = STR_X_IN_THE_WAY;
            auto ft = Formatter(res.ErrorMessageArgs.data());
            rct_string_id stringId = sceneryEntry != nullptr ? sceneryEntry->name : static_cast<rct_string_id>(STR_EMPTY);
            ft.Add<rct_string_id>(stringId);
            break;
        }
        case TILE_ELEMENT_TYPE_ENTRANCE:
            switch (tileElement->AsEntrance()->GetEntranceType())
            {
                case ENTRANCE_TYPE_RIDE_ENTRANCE:
                    res.ErrorMessage = STR_RIDE_ENTRANCE_IN_THE_WAY;
                    break;
                case ENTRANCE_TYPE_RIDE_EXIT:
                    res.ErrorMessage = STR_RIDE_EXIT_IN_THE_WAY;
                    break;
                case ENTRANCE_TYPE_PARK_ENTRANCE:
                    res.ErrorMessage = STR_PARK_ENTRANCE_IN_THE_WAY;
                    break;
            }
            break;
        case TILE_ELEMENT_TYPE_WALL:
        {
            auto* wallEntry = tileElement->AsWall()->GetEntry();
            res.ErrorMessage = STR_X_IN_THE_WAY;
            auto ft = Formatter(res.ErrorMessageArgs.data());
            rct_string_id stringId = wallEntry != nullptr ? wallEntry->name : static_cast<rct_string_id>(STR_EMPTY);
            ft.Add<rct_string_id>(stringId);
            break;
        }
        case TILE_ELEMENT_TYPE_LARGE_SCENERY:
        {
            auto* sceneryEntry = tileElement->AsLargeScenery()->GetEntry();
            res.ErrorMessage = STR_X_IN_THE_WAY;
            auto ft = Formatter(res.ErrorMessageArgs.data());
            rct_string_id stringId = sceneryEntry != nullptr ? sceneryEntry->name : static_cast<rct_string_id>(STR_EMPTY);
            ft.Add<rct_string_id>(stringId);
            break;
        }
    }
}
