/**
 * C++ implementation of the ipp module.
 */
#include "ipp.h"

// Always execute assert()s!
#undef NDEBUG

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>

namespace {

template<typename T>
T
readInt(std::ifstream& file) {
    T val;
    file.read(reinterpret_cast<char*>(&val), sizeof(val));
    if (!file.good()) {
        throw std::runtime_error("Unexpected EOF");
    }
    return val;
}

std::string
readString(std::ifstream& file) {
    std::string s;
    std::getline(file, s, '\0');
    if (!file.good()) {
        throw std::runtime_error("Unexpected EOF");
    }
    return s;
}

} // namespace

void
Ipp::loadPwalns(std::string const& fileName) {
    // Reads the chromosomes and pwalns from the given file.
    // The data in the file is expected to be in the following format:
    //
    // num_chomosomes            [uint16]
    // {
    //   chrom_name              [null-terminated string]
    // } num_chromosomes times
    // num_sp1                   [uint8]
    // {
    //   sp1_name                [null-terminated string]
    //   num_sp2                 [uint8]
    //   {
    //     sp2_name              [null-terminated string]
    //     num_ref_chrom_entries [uint32]
    //     {
    //       num_pwaln_entries   [uint32]
    //       {
    //         ref_start         [uint32]
    //         ref_end           [uint32]
    //         qry_start         [uint32]
    //         qry_end           [uint32]
    //         ref_chrom         [uint16]
    //         qry_chrom         [uint16]
    //       } num_pwaln_entries times
    //     } num_ref_chrom_entries times
    //   } num_sp2 times
    // } num_sp1 times
    chroms_.clear();
    pwalns_.clear();

    std::ifstream file(fileName, std::ios::in|std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("could not open the file");
    }

    // Read the chromosomes.
    auto const numChromosomes(readInt<uint16_t>(file));
    chroms_.reserve(numChromosomes);
    for (unsigned i(0); i < numChromosomes; ++i) {
        chroms_.push_back(readString(file));
    }

    // Read the pwalns.
    auto const numSp1(readInt<uint8_t>(file));
    for (unsigned i(0); i < numSp1; ++i) {
        // Create new entry in the map if it is not yet there.
        std::string const sp1(readString(file));
        auto& pwalnsSp1(pwalns_[sp1]);

        auto const numSp2(readInt<uint8_t>(file));
        for (unsigned j(0); j < numSp2; ++j) {
            std::string const sp2(readString(file));
            auto& pwaln(pwalnsSp1[sp2]);

            auto const numRefChromEntries(readInt<uint32_t>(file));
            for (unsigned k(0); k < numRefChromEntries; ++k) {
                auto const numPwalnEntries(readInt<uint32_t>(file));

                // Bulk-read the pwaln entries directly into the pwlanEntries
                // vector.
                std::vector<PwalnEntry> pwalnEntries(numPwalnEntries);
                file.read(reinterpret_cast<char*>(pwalnEntries.data()),
                          numPwalnEntries*sizeof(PwalnEntry));
                if (!file.good()) {
                    throw std::runtime_error("Unexpected EOF");
                }

                pwaln.insert({pwalnEntries[0].refChrom,
                              std::move(pwalnEntries)});
            }
        }
    }

    if (file.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error("Remaining data at EOF");
        // There is more data to read when we don't expect it.
    }
}

void
Ipp::loadGenomeSizes(std::string const& dirName) {
    // Reads the genome sizes from the files in the given directory.
    genomeSizes_.clear();

    for (auto const& it : pwalns_) {
        std::string const& species(it.first);
        std::string const fileName(dirName + "/" + species + ".sizes");
        std::ifstream file(fileName, std::ios::in);
        if (!file.is_open()) {
            throw std::runtime_error("could not open the file " + fileName);
        }

        unsigned genomeSize(0);
        for (std::string line; std::getline(file, line); ) {
            auto const spacePos(line.find('\t'));
            if (spacePos == std::string::npos) {
                throw std::runtime_error("line with no tabstop in " + fileName);
            }
            genomeSize += std::atoi(line.c_str()+spacePos);
        }
        genomeSizes_[species] = genomeSize;
    }
}

void
Ipp::setHalfLifeDistance(unsigned halfLifeDistance) {
    // Sets the half-life distance;
    halfLifeDistance_ = halfLifeDistance;
}

uint16_t
Ipp::chromIdFromName(std::string const& chromName) const {
    // Looks up the given chromosome name in chroms_ and returns its id.
    auto const it(std::find(chroms_.begin(), chroms_.end(), chromName));
    if (it == chroms_.end()) {
        throw std::runtime_error("Unknown chromosome: " + chromName);
    }
    return std::distance(chroms_.begin(), it);
}

std::string const&
Ipp::chromName(uint16_t chromId) const {
    // Returns the name of the chromosome with the given id.
    return chroms_.at(chromId);
}

double
Ipp::getScalingFactor(unsigned genomeSize) const {
    // Returns the scaling factor that produces a score of 0.5 for
    // halfLifeDistance_ in the reference species.
    // This scaling factor will be used in all other species in the graph, but
    // scaled to the according respective genome sizes.
    return -1.0d * halfLifeDistance_ / (genomeSize * std::log(0.5));
}

void
Ipp::projectCoords(
    std::string const& refSpecies,
    std::string const& qrySpecies,
    std::vector<Coords> const& refCoords,
    unsigned const nCores,
    OnProjectCoordsJobDoneCallback const& onJobDoneCallback) const {

    std::mutex mutex;
    std::vector<Coords> jobs(refCoords);

    bool hadWorkerException(false);
    std::exception workerException;

    auto const worker = [&]() {
        while (true) {
            // Get the next job.
            Coords refCoord;
            {
                std::lock_guard const lockGuard(mutex);
                if (jobs.empty()) {
                    // All jobs done.
                    return;
                }

                refCoord = jobs.back();
                jobs.pop_back();
            }

            // Execute the next job.
            Ipp::CoordProjection coordProjection;
            try {
                coordProjection =
                    projectCoord(refSpecies, qrySpecies, refCoord);
            } catch (std::exception const& e) {
                {
                    std::lock_guard const lockGuard(mutex);
                    hadWorkerException = true;
                    workerException = e;
                    return;
                }
            }

            // Call the callback.
            {
                std::lock_guard const lockGuard(mutex);
                onJobDoneCallback(refCoord, coordProjection);
            }
        }
    };

    if (nCores <= 1) {
        worker();
        // Just execute the worker in this thread.
    } else {
        // Create the threads.
        std::vector<std::thread> threads;
        for (unsigned i(0); i < nCores; ++i) {
            threads.emplace_back(worker);
        }

        // Wait for the threads to complete.
        for (auto& thread : threads) {
            thread.join();
        }

        // Forward any exception that occured in a thread.
        if (hadWorkerException) {
            throw workerException;
        }
    }
}

Ipp::CoordProjection
Ipp::projectCoord(std::string const& refSpecies,
                  std::string const& qrySpecies,
                  Coords const& refCoords) const {
    bool const debug(false);
    if (debug) {
        std::cout.precision(16);
        std::cout << std::endl;
        std::cout << refSpecies << " " << qrySpecies << " "
                  << refCoords.chrom << ":" << refCoords.loc << std::endl;
    }

    double const scalingFactor(getScalingFactor(genomeSizes_.at(refSpecies)));

    CoordProjection coordProjection;
    ShortestPath& shortestPath(coordProjection.multiShortestPath);
    shortestPath[refSpecies] = { 1.0, "", refCoords, {} };

    struct OrangeEntry {
        double score;
        std::string species;
        Ipp::Coords coords;

        OrangeEntry(double score,
                    std::string const& species,
                    Ipp::Coords const& coords)
            : score(score), species(species), coords(coords)
        {}

        bool operator<(OrangeEntry const& other) const {
            return std::tie(score, species, coords)
                < std::tie(other.score, other.species, other.coords);
        }
    };
    std::priority_queue<OrangeEntry> orange;
    orange.emplace(1.0, refSpecies, refCoords);

    while (!orange.empty()) {
        OrangeEntry const current(orange.top());
        orange.pop();

        auto it(shortestPath.find(current.species));
        if (it != shortestPath.end() && it->second.score > current.score) {
            continue;
            // The current species was already reached by a faster path, ignore
            // this path and go to the next species.
        }

        if (debug) {
            std::cout << "- " << current.species << " " << current.score << " "
                      << current.coords.chrom << ":" << current.coords.loc
                      << std::endl;
        }
        
        if (current.species == qrySpecies) {
            break;
            // qry species reached, stop.
        }

        for (auto const& nxt : pwalns_.at(current.species)) {
            std::string const& nxtSpecies(nxt.first);
            it = shortestPath.find(nxtSpecies);
            if (it != shortestPath.end()
                && current.score <= it->second.score) {
                continue;
                // If the score to the current species is lower than any
                // previous path to nxtSpecies, then nxtSpecies won't be reached
                // faster through the current species.
            }

            if (debug) {
                std::cout << "--> " << nxtSpecies << std::endl;
            }

            auto const proj(projectGenomicLocation(current.species,
                                                   nxtSpecies,
                                                   current.coords,
                                                   scalingFactor));
            if (!proj) {
                continue;
                // No path was found.
            }

            if(current.species == refSpecies && nxtSpecies == qrySpecies) {
                // Direct projection.
                coordProjection.direct = *proj;
            }

            double const nxtScore(current.score * proj->score);
            if (it != shortestPath.end() && nxtScore <= it->second.score) {
                continue;
                // Only save the current path to nxtSpecies if it is faster than
                // any previous path to it.
            }

            shortestPath[nxtSpecies] = {
                nxtScore,
                current.species,
                proj->nextCoords,
                proj->anchors
            };
            orange.emplace(nxtScore, nxtSpecies, proj->nextCoords);
        }
    }

    return coordProjection;
}

namespace {

template<typename T>
void
updateChromCount(std::unordered_map<uint16_t, unsigned>& chromCount,
                 T const& v) {
    for (Ipp::PwalnEntry const& entry : v) {
        ++chromCount[entry.qryChrom];
    }
}

template<typename T, typename... Args>
void
updateChromCount(std::unordered_map<uint16_t, unsigned>& chromCount,
                 T const& v,
                 Args const&... args) {
    updateChromCount(chromCount, v);
    updateChromCount(chromCount, args...);
}

template<typename... Args>
uint16_t
computeMajorChrom(Args const&... args) {
    std::unordered_map<uint16_t, unsigned> chromCount;
    updateChromCount(chromCount, args...);

    unsigned maxCount(0);
    uint16_t maxChrom(0);
    for (auto const& [chrom, count] : chromCount) {
        if (count > maxCount) {
            maxChrom = chrom;
            maxCount = count;
        }
    }
    return maxChrom;
}

template<typename T>
void
removeEntriesWithNonMajorQryChromosome(T& anchors, uint16_t majorChrom) {
    for (auto it(anchors.begin()); it != anchors.end(); ) {
        if (it->qryChrom != majorChrom) {
            it = anchors.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

std::optional<Ipp::GenomicProjectionResult>
Ipp::projectGenomicLocation(std::string const& refSpecies,
                            std::string const& qrySpecies,
                            Coords const& refCoords,
                            double scalingFactor) const {
    auto const it1(pwalns_.find(refSpecies));
    if (it1 == pwalns_.end()) {
        // There is no pairwise alignment for the ref species.
        return {};
    }
    auto const it2(it1->second.find(qrySpecies));
    if (it2 == it1->second.end()) {
        // There is no pairwise alignment for the qry species.
        return {};
    }

    auto const anchors(getAnchors(it2->second, refCoords));
    if (!anchors) {
        // If no or only one anchor is found because of border region, return 0
        // score and empty coordinate string.
        return {};
    }

    uint32_t const refLoc(refCoords.loc);

    // Compute the qryLoc by linear interpolation: Consider where refLoc lies
    // between the ref coords of the up- and downstream anchors and project that
    // to the qry coords of the anchors.
    uint32_t refLeftBound, refRightBound, qryLeftBound, qryRightBound;
    // The qry coords might be reversed (start > end). If the upstream
    // anchor is reversed then the downstream anchor is, too.
    bool const isQryReversed(anchors->upstream.isQryReversed());
    uint32_t const qryUpStart(!isQryReversed
                              ? anchors->upstream.qryStart
                              : anchors->downstream.qryEnd);
    uint32_t const qryUpEnd(!isQryReversed
                            ? anchors->upstream.qryEnd
                            : anchors->downstream.qryStart);
    assert(qryUpStart < qryUpEnd);
    double score;
    if (anchors->upstream == anchors->downstream) {
        // refLoc lies on an aligment.
        //  [  up.ref  ]
        //  [ down.ref ]
        //          x
        refLeftBound = anchors->upstream.refStart;
        refRightBound = anchors->upstream.refEnd;
        qryLeftBound = qryUpStart;
        qryRightBound = qryUpEnd;

        score = 1.0d;
    } else {
        // [ up.ref ]  x    [ down.ref ]
        uint32_t const qryDownStart(!isQryReversed
                                    ? anchors->downstream.qryStart
                                    : anchors->upstream.qryEnd);
        uint32_t const qryDownEnd(!isQryReversed
                                  ? anchors->downstream.qryEnd
                                  : anchors->upstream.qryStart);
        assert(qryUpEnd <= qryDownStart && qryDownStart < qryDownEnd);

        refLeftBound= anchors->upstream.refEnd;
        refRightBound = anchors->downstream.refStart; 
        qryLeftBound = qryUpEnd;
        qryRightBound = qryDownStart;

        score = projectionScore(refLoc,
                                refLeftBound,
                                refRightBound,
                                genomeSizes_.at(refSpecies),
                                scalingFactor);
    }
    assert(refLeftBound <= refLoc && refLoc < refRightBound);
    double const relativeRefLoc(
        1.0d*(refLoc - refLeftBound) / (refRightBound - refLeftBound));
    uint32_t const qryLoc(
        qryLeftBound + relativeRefLoc*(qryRightBound - qryLeftBound));
    // ONLY USE DISTANCE TO CLOSE ANCHOR AT REF SPECIES, because at the qry
    // species it should be roughly the same as it is a projection of the
    // reference.

    return {{score, {anchors->upstream.qryChrom, qryLoc}, *anchors}};
}

std::optional<Ipp::Anchors>
Ipp::getAnchors(Pwaln const& pwaln, Coords const& refCoords) const {
    // First define anchors upstream, downstream and ovAln, then do major-chrom
    // and collinearity test, then either return overlapping anchor or closest
    // anchors.
    // Take orientation into account for the anchor definition. If start > end,
    // then the aln is to the '-' strand.
    // For speed reasons only select the first `topn` entries. The rest just
    // takes longer to compute min / max and most likely will (and should) not
    // be an anchor anyways.

    // Test collinearity of anchors: take top 20 in each direction (top 10 
    // produced many locally collinear pwalns that were still non-collinear
    // outliers in the global view of the GRB).
    // Note: using ungapped chain blocks might require n to be even larger.
    unsigned const minn(5);
    unsigned const topn(20);

    auto const compGreaterRefEnd = [](PwalnEntry const& lhs,
                                      PwalnEntry const& rhs) {
        return lhs.refEnd > rhs.refEnd;
    };

    uint32_t const refLoc(refCoords.loc);

    // Find the topn entries by largest(smallest) refEnd(refStart) in the
    // upstream(downstream) anchors.
    std::set<PwalnEntry, decltype(compGreaterRefEnd)> anchorsUpstream(
        compGreaterRefEnd);
    std::vector<PwalnEntry> ovAln;
    std::vector<PwalnEntry> anchorsDownstream;
    for (auto const& pwalnEntry : pwaln.at(refCoords.chrom)) {
        if (pwalnEntry.refEnd <= refLoc) { // refEnd is exclusive
            // upstream anchor
            // [ anchor ]    x
            anchorsUpstream.insert(pwalnEntry);
            if(anchorsUpstream.size() > 10*topn) {
                // Remove surplus anchors that are too far away. We do that
                // heuristically once we reach 10 times the maximum number to
                // amortize the cost.
                anchorsUpstream.erase(std::next(anchorsUpstream.begin(), topn),
                                      anchorsUpstream.end());
            }
        } else if (refLoc < pwalnEntry.refStart) {
            // downstream anchor
            //    x     [ anchor ]
            anchorsDownstream.push_back(pwalnEntry);
            if(anchorsDownstream.size() == topn) {
                // We found the topn closest anchors with refStart > refLoc.
                // Since the pwalnEntries are sorted by refStart, all the
                // anchers to come are further away than what we have already
                // seen.
                break;
            }
        } else {
            // refLoc lies on an alignment block.
            // [ anchor ]
            //      x
            ovAln.push_back(pwalnEntry);
        }
    }

    // Trim anchorsUpstream to only contain the topn closest entries.
    if(anchorsUpstream.size() > topn) {
        anchorsUpstream.erase(std::next(anchorsUpstream.begin(), topn),
                              anchorsUpstream.end());
    }

    // MAJOR CHROMOSOME: Retain anchors that point to the majority chromosome in
    // top n of both up- and downstream anchors.
    uint16_t const majorChrom(
        computeMajorChrom(ovAln, anchorsUpstream, anchorsDownstream));
    removeEntriesWithNonMajorQryChromosome(anchorsUpstream, majorChrom);
    removeEntriesWithNonMajorQryChromosome(ovAln, majorChrom);
    removeEntriesWithNonMajorQryChromosome(anchorsDownstream, majorChrom);
  
    if (!anchorsUpstream.size() || !anchorsDownstream.size()) {
        // Require minimum of 1 anchor on each side. Later, the minimum total
        // number of collinear anchors will be set to `minn` (but one side is
        // allowed to have as little as 1 anchor).
        return {};
    }

    // COLLINEARITY: Remove pwalns pointing to outliers by getting the longest
    // sorted subsequence of the top n of both up- and downstream anchors.
    std::vector<PwalnEntry> closestAnchors;
    closestAnchors.insert(closestAnchors.begin(), anchorsUpstream.begin(), anchorsUpstream.end());
    closestAnchors.insert(closestAnchors.end(), ovAln.begin(), ovAln.end());
    closestAnchors.insert(closestAnchors.end(), anchorsDownstream.begin(), anchorsDownstream.end());

    // Sort the closestAnchors entries by increasing refStart. That is necessary
    // as the anchorsUpstream were previously sorted by decreasing refEnd.
    auto const compLessRefStart = [](PwalnEntry const& lhs,
                                     PwalnEntry const& rhs) {
        return std::tie(lhs.refStart, lhs.refEnd)
            < std::tie(rhs.refStart, rhs.refEnd);
    };
    std::sort(closestAnchors.begin(), closestAnchors.end(), compLessRefStart);

    closestAnchors = longestSubsequence(closestAnchors);

    // Set minimum number of collinear anchors to `minn` (for species pairs with
    // very large evol. distances setting a lower boundary for the number of
    // collinear anchors will help reduce false positives).
    if (closestAnchors.size() < minn) {
        return {};
    }
  
    // Check if the original ovAln is still present (or ever was) in the
    // filtered closestAnchors (that include a potential ovAln);
    // if not, it was an outlier alignment and was filtered out
    // if yes, narrow it to the actual position of refLoc and its relative
    // position in the qry such that the returned anchors have distance = 0 to
    // refLoc.
    PwalnEntry const* closestUpstreamAnchor(nullptr);
    PwalnEntry const* closestOvAlnAnchor(nullptr);
    PwalnEntry const* closestDownstreamAnchor(nullptr);
    for (auto const& anchor : closestAnchors) {
        if (anchor.refEnd <= refLoc) {
            if (!closestUpstreamAnchor
                || closestUpstreamAnchor->refEnd < anchor.refEnd) {
                closestUpstreamAnchor = &anchor;
            }
        } else if (refLoc < anchor.refStart) {
            if (!closestDownstreamAnchor
                || anchor.refStart < closestDownstreamAnchor->refStart) {
                closestDownstreamAnchor = &anchor;

                // The anchors that follow this one will only be worse.
                break;
            }
        } else {
            if(!closestOvAlnAnchor) {
                closestOvAlnAnchor = &anchor;
            } else {
                auto const absDiff = [](uint32_t a, uint32_t b) {
                    return a > b ? a-b : b-a;
                };
                uint32_t const minDistNew(
                    std::min(absDiff(closestOvAlnAnchor->refStart, refLoc),
                             absDiff(closestOvAlnAnchor->refEnd, refLoc)));
                uint32_t const minDistOld(
                    std::min(absDiff(anchor.refStart, refLoc),
                             absDiff(anchor.refEnd, refLoc)));
                if (minDistNew < minDistOld) {
                    closestOvAlnAnchor = &anchor;
                }
            }
        }
    }
    if (closestOvAlnAnchor) {
        //uint32_t const locRelative(refLoc - closestOvAlnAnchor->refStart);
        //bool const isPositiveStrand(
        //    closestOvAlnAnchor->qryStart < closestOvAlnAnchor->qryEnd);

        //PwalnEntry anchorUp(*closestOvAlnAnchor);
        //anchorUp.refStart = refLoc - 1;
        //anchorUp.refEnd = refLoc;
        //anchorUp.qryStart = isPositiveStrand
        //    ? closestOvAlnAnchor->qryStart + locRelative - 1
        //    : closestOvAlnAnchor->qryStart - locRelative + 2;
        //anchorUp.qryEnd = isPositiveStrand
        //    ? closestOvAlnAnchor->qryStart + 1
        //    : closestOvAlnAnchor->qryStart - 1;
    
        //PwalnEntry anchorDown(anchorUp);
        //anchorDown.refStart += 1;
        //anchorDown.refEnd += 1;
        //anchorDown.qryStart += isPositiveStrand ? 1 : -1;
        //anchorDown.qryEnd += isPositiveStrand ? 1 : -1;

        //return {{anchorUp, anchorDown}};
        return {{*closestOvAlnAnchor, *closestOvAlnAnchor}};
    } else {
        if (!closestUpstreamAnchor || !closestDownstreamAnchor) {
            // Not both up- and downstream anchors were found (e.g. at synteny
            // break points where one side does not have any anchors to the
            // majority chromosome)
            return {};
        }

        return {{*closestUpstreamAnchor, *closestDownstreamAnchor}};
    }
}

namespace {

std::vector<Ipp::PwalnEntry>
longestSubsequence(std::vector<Ipp::PwalnEntry> const& seq,
                   std::function<bool(Ipp::PwalnEntry const&)> const& filter,
                   std::function<int(Ipp::PwalnEntry const&)> const& qryStart,
                   std::function<int(Ipp::PwalnEntry const&)> const& qryEnd) {
    // Finds the longest strictly increasing subsequence (in regards to the
    // given greater-than function). Only elements for which filter(seq[i])
    // returns true are considered.
    // O(n log k) algorithm.

    // m[i] contains the index to the smallest value in seq[] that is the end of
    // a subsequence of length i+1.
    std::vector<unsigned> m;
    m.reserve(seq.size());

    if (!seq.size()) {
        return {};
    }

    // prev[i] contains the index of the element in seq that is the one before
    // seq[i] in the longest subsequence for seq[i].
    std::vector<unsigned> prev(seq.size());

    for (unsigned i(0); i < seq.size(); ++i) {
        // If the next element seq[i] is greater than the last element of the
        // current longest subsequence seq[m.back()], just push it to the end of
        // `m` and continue.
        if (!filter(seq[i])) {
            continue;
        }

        if (!m.size()) {
            // This is the first element that matches the filter. Just add it to
            // m.
            m.push_back(i);
            continue;
        }

        if (qryEnd(seq[m.back()]) <= qryStart(seq[i])) {
            prev[i] = m.back();
            m.push_back(i);
            continue;
        }

        // Binary search to find the smallest element referenced by m which is
        // just bigger than seq[i].
        // Note : Binary search is performed on m (and not seq).
        // Size of m is always <=i and hence contributes O(log i) to the
        // complexity.
        unsigned u(0);
        unsigned v(m.size()-1);
        while(u < v) {
            unsigned const mid((u + v) / 2);
            if (qryEnd(seq[m[mid]]) <= qryStart(seq[i])) {
                u = mid+1;
            } else {
                v = mid;
            }
        }

        // Update m if the new value is smaller than the previously referenced
        // one.
        if (qryEnd(seq[i]) < qryEnd(seq[m[u]])) {
            if (u > 0) {
                prev[i] = m[u-1];
            }
            m[u] = i;
        }
    }

    // Backtrace the longest subsequence into res.
    std::vector<Ipp::PwalnEntry> res(m.size());
    unsigned v(m.back());
    for (unsigned u(m.size()); u; --u) {
        res[u-1] = seq[v];
        v = prev[v];
    }
    return res;
}

} // namespace

std::vector<Ipp::PwalnEntry>
Ipp::longestSubsequence(std::vector<PwalnEntry> const& seq) {
    bool const debug(false);

    auto const printSeq = [](std::vector<PwalnEntry> const& seq) {
        for (auto const& e : seq) {
            std::cerr << e.qryStart << ' ' << e.qryEnd << ' '
                      << (e.isQryReversed() ? "(-)" : "(+)") << std::endl;
        }
    };
    if (debug) {
        std::cerr << "seq: " << std::endl;
        printSeq(seq);
        std::cerr << std::endl;
    }

    std::vector<PwalnEntry> const inc(::longestSubsequence(
            seq,
            [](PwalnEntry const& e) {
                return !e.isQryReversed();
            },
            [](PwalnEntry const& e) {
                return e.qryStart;
            },
            [](PwalnEntry const& e) {
                return e.qryEnd;
            }));

    std::vector<PwalnEntry> const dec(::longestSubsequence(
            seq,
            [](PwalnEntry const& e) {
                return e.isQryReversed();
            },
            [](PwalnEntry const& e) {
                return -1 * (int)e.qryStart;
            },
            [](PwalnEntry const& e) {
                return -1 * (int)e.qryEnd;
            }));

    if (debug) {
        std::cerr << "inc: " << std::endl;
        printSeq(inc);
        std::cerr << std::endl;

        std::cerr << "dec: " << std::endl;
        printSeq(dec);
        std::cerr << std::endl
                  << "---------------------------" << std::endl
                  << std::endl;
    }

    // Sanity check: The entries in the inc/dec list should be strictly
    // increasing/decreasing.
    uint32_t loc(0);
    for (auto const& e : inc) {
        assert(loc <= e.qryStart);
        assert(e.qryStart < e.qryEnd);
        loc = e.qryEnd;
    }
    loc = std::numeric_limits<uint32_t>::max();
    for (auto const& e : dec) {
        assert(loc >= e.qryEnd);
        assert(e.qryStart > e.qryEnd);
        loc = e.qryStart;
    }

    return inc.size() >= dec.size() ? inc : dec;
}

double
Ipp::projectionScore(uint32_t loc,
                     uint32_t leftBound,
                     uint32_t rightBound,
                     unsigned genomeSize,
                     double scalingFactor) const {
    // Anchors must be the locations of the up- and downstream anchors, not the
    // data frame with ref and qry coordinates.
    // The scaling factor determines how fast the function falls when moving
    // away from an anchor.
    // Ideally, we define a half-life X_half, i.e. at a distance of X_half, the
    // model is at 0.5. With a scaling factor of 50 kb, X_half is at 20 kb (with
    // 100 kb at 10 kb).
    uint32_t const d(std::min(loc - leftBound, rightBound - loc));
    return std::exp(-1.0d * d / (genomeSize * scalingFactor));
}
