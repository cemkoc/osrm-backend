#include "extractor/guidance/intersection_normalizer.hpp"
#include "extractor/guidance/toolkit.hpp"
#include "util/guidance/toolkit.hpp"

namespace osrm
{
namespace extractor
{
namespace guidance
{

IntersectionNormalizer::IntersectionNormalizer(
    const util::NodeBasedDynamicGraph &node_based_graph,
    const std::vector<extractor::QueryNode> &node_coordinates,
    const util::NameTable &name_table,
    const SuffixTable &street_name_suffix_table,
    const IntersectionGenerator &intersection_generator)
    : node_based_graph(node_based_graph), node_coordinates(node_coordinates),
      name_table(name_table), street_name_suffix_table(street_name_suffix_table),
      intersection_generator(intersection_generator),
      mergable_road_detector(node_based_graph,
                             node_coordinates,
                             intersection_generator,
                             intersection_generator.GetCoordinateExtractor())
{
}

Intersection IntersectionNormalizer::operator()(const NodeID node_at_intersection,
                                                Intersection intersection) const
{
    return AdjustForJoiningRoads(
        node_at_intersection, MergeSegregatedRoads(node_at_intersection, std::move(intersection)));
}

// Checks for mergability of two ways that represent the same intersection. For further
// information
// see interface documentation in header.
bool IntersectionNormalizer::CanMerge(const NodeID node_at_intersection,
                                      const Intersection &intersection,
                                      std::size_t first_index,
                                      std::size_t second_index) const
{
    const auto &first_data = node_based_graph.GetEdgeData(intersection[first_index].eid);
    const auto &second_data = node_based_graph.GetEdgeData(intersection[second_index].eid);

    // don't merge on degree two, since it's most likely a bollard/traffic light or a round way
    if (intersection.size() <= 2)
        return false;

    // only merge named ids
    if (first_data.name_id == EMPTY_NAMEID || second_data.name_id == EMPTY_NAMEID)
        return false;

    // need to be same name
    if (util::guidance::requiresNameAnnounced(
            first_data.name_id, second_data.name_id, name_table, street_name_suffix_table))
        return false;

    if (!mergable_road_detector.CanMergeRoad(
            node_at_intersection, intersection[first_index], intersection[second_index]))
        return false;

    else
        return true;
}

/*
 * Segregated Roads often merge onto a single intersection.
 * While technically representing different roads, they are
 * often looked at as a single road.
 * Due to the merging, turn Angles seem off, wenn we compute them from the
 * initial positions.
 *
 *         b<b<b<b(1)<b<b<b
 * aaaaa-b
 *         b>b>b>b(2)>b>b>b
 *
 * Would be seen as a slight turn going fro a to (2). A Sharp turn going from
 * (1) to (2).
 *
 * In cases like these, we megre this segregated roads into a single road to
 * end up with a case like:
 *
 * aaaaa-bbbbbb
 *
 * for the turn representation.
 * Anything containing the first u-turn in a merge affects all other angles
 * and is handled separately from all others.
 */
Intersection IntersectionNormalizer::MergeSegregatedRoads(const NodeID intersection_node,
                                                          Intersection intersection) const
{
    const auto getRight = [&](std::size_t index) {
        return (index + intersection.size() - 1) % intersection.size();
    };

    // we only merge small angles. If the difference between both is large, we are looking at a
    // bearing leading north. Such a bearing cannot be handled via the basic average. In this
    // case we actually need to shift the bearing by half the difference.
    const auto aroundZero = [](const double first, const double second) {
        return (std::max(first, second) - std::min(first, second)) >= 180;
    };

    // find the angle between two other angles
    const auto combineAngles = [aroundZero](const double first, const double second) {
        if (!aroundZero(first, second))
            return .5 * (first + second);
        else
        {
            const auto offset = angularDeviation(first, second);
            auto new_angle = std::max(first, second) + .5 * offset;
            if (new_angle > 360)
                return new_angle - 360;
            return new_angle;
        }
    };

    const auto merge = [combineAngles](const ConnectedRoad &first,
                                       const ConnectedRoad &second) -> ConnectedRoad {
        ConnectedRoad result = first.entry_allowed ? first : second;
        result.angle = combineAngles(first.angle, second.angle);
        result.bearing = combineAngles(first.bearing, second.bearing);
        BOOST_ASSERT(0 <= result.angle && result.angle <= 360.0);
        BOOST_ASSERT(0 <= result.bearing && result.bearing <= 360.0);
        return result;
    };

    if (intersection.size() <= 1)
        return intersection;

    const bool is_connected_to_roundabout = [this, &intersection]() {
        for (const auto &road : intersection)
        {
            if (node_based_graph.GetEdgeData(road.eid).roundabout)
                return true;
        }
        return false;
    }();

    // check for merges including the basic u-turn
    // these result in an adjustment of all other angles. This is due to how these angles are
    // perceived. Considering the following example:
    //
    //   c   b
    //     Y
    //     a
    //
    // coming from a to b (given a road that splits at the fork into two one-ways), the turn is not
    // considered as a turn but rather as going straight.
    // Now if we look at the situation merging:
    //
    //  a     b
    //    \ /
    // e - + - d
    //     |
    //     c
    //
    // With a,b representing the same road, the intersection itself represents a classif for way
    // intersection so we handle it like
    //
    //   (a),b
    //      |
    // e -  + - d
    //      |
    //      c
    //
    // To be able to consider this adjusted representation down the line, we merge some roads.
    // If the merge occurs at the u-turn edge, we need to adjust all angles, though, since they are
    // with respect to the now changed perceived location of a. If we move (a) to the left, we add
    // the difference to all angles. Otherwise we subtract it.
    bool merged_first = false;
    // these result in an adjustment of all other angles
    if (CanMerge(intersection_node, intersection, 0, intersection.size() - 1))
    {
        merged_first = true;
        // moving `a` to the left
        const double correction_factor = (360 - intersection[intersection.size() - 1].angle) / 2;
        for (std::size_t i = 1; i + 1 < intersection.size(); ++i)
            intersection[i].angle += correction_factor;

        // FIXME if we have a left-sided country, we need to switch this off and enable it
        // below
        intersection[0] = merge(intersection.front(), intersection.back());
        intersection[0].angle = 0;
        intersection.pop_back();
    }
    else if (CanMerge(intersection_node, intersection, 0, 1))
    {
        merged_first = true;
        // moving `a` to the right
        const double correction_factor = (intersection[1].angle) / 2;
        for (std::size_t i = 2; i < intersection.size(); ++i)
            intersection[i].angle -= correction_factor;
        intersection[0] = merge(intersection[0], intersection[1]);
        intersection[0].angle = 0;
        intersection.erase(intersection.begin() + 1);
    }

    if (merged_first && is_connected_to_roundabout)
    {
        /*
         * We are merging a u-turn against the direction of a roundabout
         *
         *     -----------> roundabout
         *        /    \
         *     out      in
         *
         * These cases have to be disabled, even if they are not forbidden specifically by a
         * relation
         */
        intersection[0].entry_allowed = false;
    }

    // a merge including the first u-turn requires an adjustment of the turn angles
    // therefore these are handled prior to this step
    for (std::size_t index = 2; index < intersection.size(); ++index)
    {
        if (CanMerge(intersection_node, intersection, index, getRight(index)))
        {
            intersection[getRight(index)] =
                merge(intersection[getRight(index)], intersection[index]);
            intersection.erase(intersection.begin() + index);
            --index;
        }
    }

    std::sort(std::begin(intersection),
              std::end(intersection),
              std::mem_fn(&ConnectedRoad::compareByAngle));
    return intersection;
}

// OSM can have some very steep angles for joining roads. Considering the following intersection:
//        x
//        |
//        v __________c
//       /
// a ---d
//       \ __________b
//
// with c->d as a oneway
// and d->b as a oneway, the turn von x->d is actually a turn from x->a. So when looking at the
// intersection coming from x, we want to interpret the situation as
//           x
//           |
// a __ d __ v__________c
//      |
//      |_______________b
//
// Where we see the turn to `d` as a right turn, rather than going straight.
// We do this by adjusting the local turn angle at `x` to turn onto `d` to be reflective of this
// situation, where `v` would be the node at the intersection.
Intersection IntersectionNormalizer::AdjustForJoiningRoads(const NodeID node_at_intersection,
                                                           Intersection intersection) const
{
    // nothing to do for dead ends
    if (intersection.size() <= 1)
        return intersection;

    const util::Coordinate coordinate_at_intersection = node_coordinates[node_at_intersection];
    // never adjust u-turns
    for (std::size_t index = 1; index < intersection.size(); ++index)
    {
        auto &road = intersection[index];
        // to find out about the above situation, we need to look at the next intersection (at d in
        // the example). If the initial road can be merged to the left/right, we are about to adjust
        // the angle.
        const auto next_intersection_along_road =
            intersection_generator(node_at_intersection, road.eid);

        if (next_intersection_along_road.size() <= 1)
            continue;

        const auto node_at_next_intersection = node_based_graph.GetTarget(road.eid);
        const util::Coordinate coordinate_at_next_intersection =
            node_coordinates[node_at_next_intersection];
        if (util::coordinate_calculation::haversineDistance(coordinate_at_intersection,
                                                            coordinate_at_next_intersection) > 30)
            continue;

        const auto adjustAngle = [](double angle, double offset) {
            angle += offset;
            if (angle > 360)
                return angle - 360.;
            else if (angle < 0)
                return angle + 360.;
            return angle;
        };

        const auto range = node_based_graph.GetAdjacentEdgeRange(node_at_next_intersection);
        if (range.size() <= 1)
            continue;

        // the order does not matter
        const auto get_offset = [](const ConnectedRoad &lhs, const ConnectedRoad &rhs) {
            return 0.5 * angularDeviation(lhs.angle, rhs.angle);
        };

        // When offsetting angles in our turns, we don't want to get past the next turn. This
        // function simply limits an offset to be at most half the distance to the next turn in the
        // offfset direction
        const auto get_corrected_offset = [](const double offset,
                                             const ConnectedRoad &road,
                                             const ConnectedRoad &next_road_in_offset_direction) {
            const auto offset_limit =
                angularDeviation(road.angle, next_road_in_offset_direction.angle);
            // limit the offset with an additional buffer
            return (offset + MAXIMAL_ALLOWED_NO_TURN_DEVIATION > offset_limit) ? 0.5 * offset_limit
                                                                               : offset;
        };

        // check if the u-turn edge at the next intersection could be merged to the left/right. If
        // this is the case and the road is not far away (see previous distance check), if
        // influences the perceived angle.
        if (CanMerge(node_at_next_intersection, next_intersection_along_road, 0, 1))
        {
            const auto offset =
                get_offset(next_intersection_along_road[0], next_intersection_along_road[1]);

            const auto corrected_offset =
                get_corrected_offset(offset, road, intersection[(index + 1) % intersection.size()]);
            // at the target intersection, we merge to the right, so we need to shift the current
            // angle to the left
            road.angle = adjustAngle(road.angle, corrected_offset);
            road.bearing = adjustAngle(road.bearing, corrected_offset);
        }
        else if (CanMerge(node_at_next_intersection,
                          next_intersection_along_road,
                          0,
                          next_intersection_along_road.size() - 1))
        {
            const auto offset =
                get_offset(next_intersection_along_road[0],
                           next_intersection_along_road[next_intersection_along_road.size() - 1]);

            const auto corrected_offset =
                get_corrected_offset(offset, road, intersection[index - 1]);
            // at the target intersection, we merge to the left, so we need to shift the current
            // angle to the right
            road.angle = adjustAngle(road.angle, -corrected_offset);
            road.bearing = adjustAngle(road.bearing, -corrected_offset);
        }
    }
    return intersection;
}

} // namespace guidance
} // namespace extractor
} // namespace osrm
