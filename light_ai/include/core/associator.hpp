// Associator: one round of detection-to-track association -- the candidate
// filter, the size-consistency gate, the IoU gate, global greedy matching
// (greedy_iou_match, box_math.hpp). Two rounds over one pair buffer
// (ByteTrack-style; rounds 0/1 in the code): round 1 pairs every person det
// with every alive track at the strong gate kIoUGate; round 2 pairs
// round-1-unmatched dets with round-1-unmatched CONF tracks at the weak
// gate kIoUGateWeak, position only -- what a match does to the track (hits)
// is Tracker::associate's business. Target case: person #2 emits conf
// 0.25-0.55 dets that undershoot the strong IoU gate while their track
// coasts on damped velocity -- without round 2 the track dies at kConfMaxAgeMs
// and the person re-spawns under a new id.
//
// Size gate: a det matches only if its w/h stay within [kSizeMin, kSizeMax]
// percent of the track's EMA size. A partial det (a person half out of
// frame) is a full-height box squeezed far below that -- without the gate
// it steals the IoU match and injects a false velocity spike into a
// standing person's track. Round 2 relaxes the upper bound (kSizeMaxWeak):
// a sliver-born track stays too small to accept its person's full-size det
// at kSizeMax and used to starve to death (observed live).
//
// Pure logic on the pipeline thread (Tracker::update); the pair buffer is a
// preallocated member.

#pragma once

#include <array>

#include "core/box_math.hpp"   // IouPair
#include "core/detection.hpp"  // DetList, Detection
#include "core/track.hpp"      // TrackLife

// A track as the association sees it: the KF posterior center, the EMA
// size and the lifecycle state.
struct TrackBox
{
    float cx = 0.0f;
    float cy = 0.0f;
    float bw = 0.0f;
    float bh = 0.0f;
    TrackLife life = TRACK_FREE;
};

class Associator
{
public:
    // Tuning: x100 integer constants, one place to tune.
    enum
    {
        kMaxTracks = 16,
        kMaxDets = DetList::cap,
        kIoUGate = 30,      // min IoU x100 to accept a match
        kIoUGateWeak = 15,  // round-2 IoU gate (x100): a det unmatched in round 1
                            // vs a round-1-unmatched coasting CONF track (stage
                            // 3, ByteTrack-style second pass). Verified live 2026-11-12:
                            // no phantoms at 15, weak-det survival confirmed (4 ids / 600 frames)
        kSizeMin = 40,      // det/track size ratio gate, lower x100
        kSizeMax = 200,     // det/track size ratio gate, upper x100
        kSizeMaxWeak = 300, // round-2 det/track size-ratio gate, upper x100:
                            // a track spawned from a partial det must still be
                            // able to accept the full-size det of the same
                            // person (round 2 measures the position only, so
                            // the oversized det never enters the KF). Candidate
                            // value verified live 2026-11-12: ids 4/6-class starvation gone,
                            // no noise confirmed
    };

    using TrackBoxes = std::array<TrackBox, kMaxTracks>;
    using DetTrack = std::array<int, kMaxDets>;              // per det: -1 = unmatched, else the track index
    using TrackTaken = std::array<unsigned char, kMaxTracks>;  // per track: 1 = matched

    // round: 0 = round 1 (strong gates), 1 = round 2 (weak gates). Pairs every
    // person det (cls 0) not yet in det_trk with every candidate track not yet
    // in trk_taken, through the size and IoU gates of the round, then matches
    // them best-first. Marks the
    // matches in det_trk / trk_taken and returns their count; pair(0 .. n)
    // are the accepted pairs, in acceptance order.
    int match( int round, const DetList& dets, const TrackBoxes& tracks, DetTrack& det_trk, TrackTaken& trk_taken );
    const IouPair& pair( int i ) const { return pairs_[static_cast<size_t>( i )]; }

private:
    bool candidate( int round, const TrackBox& t ) const;
    bool gate( int round, const Detection& d, const TrackBox& t, float& iou ) const;

private:
    std::array<IouPair, static_cast<std::size_t>( kMaxDets ) * kMaxTracks> pairs_ {};
};
