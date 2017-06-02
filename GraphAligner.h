#ifndef GraphAligner_H
#define GraphAligner_H

//http://biorxiv.org/content/early/2017/04/06/124941
#include <chrono>
#include <algorithm>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <boost/config.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <boost/graph/johnson_all_pairs_shortest.hpp>
#include <boost/container/flat_set.hpp>
#include <unordered_set>
#include "vg.pb.h"
#include "2dArray.h"
#include "SliceRow.h"
#include "SparseBoolMatrix.h"
#include "SparseMatrix.h"

using namespace boost;

void printtime(const char* msg)
{
	static auto time = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
	auto newtime = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
	std::cout << msg << " " << newtime << " (" << (newtime - time) << ")" << std::endl;
	time = newtime;
}

template <typename LengthType, typename ScoreType>
class GraphAligner
{
public:
	class AlignmentResult
	{
	public:
		AlignmentResult(vg::Alignment alignment, int maxDistanceFromBand, bool alignmentFailed) :
		alignment(alignment),
		maxDistanceFromBand(maxDistanceFromBand),
		alignmentFailed(alignmentFailed)
		{
		}
		vg::Alignment alignment;
		int maxDistanceFromBand;
		bool alignmentFailed;
	};
	typedef std::pair<LengthType, LengthType> MatrixPosition;
	class MatrixSlice
	{
	public:
		MatrixSlice() :
		M(),
		Q(),
		R(),
		Rbacktrace(),
		Qbacktrace(),
		scores(1, 1)
		{}
		std::vector<ScoreType> M;
		std::vector<ScoreType> Q;
		std::vector<ScoreType> R;
		std::vector<MatrixPosition> Rbacktrace;
		std::vector<MatrixPosition> Qbacktrace;
		SparseMatrix<ScoreType> scores;
	};
	class SeedHit
	{
	public:
		SeedHit(size_t seqPos, int nodeId, size_t nodePos) : sequencePosition(seqPos), nodeId(nodeId), nodePos(nodePos) {};
		size_t sequencePosition;
		int nodeId;
		size_t nodePos;
	};

	GraphAligner() :
	nodeStart(),
	indexToNode(),
	nodeLookup(),
	nodeIDs(),
	inNeighbors(),
	nodeSequences(),
	gapStartPenalty(1),
	gapContinuePenalty(1)
	{
		//add the start dummy node as the first node
		dummyNodeStart = nodeSequences.size();
		nodeIDs.push_back(0);
		nodeStart.push_back(nodeSequences.size());
		inNeighbors.emplace_back();
		outNeighbors.emplace_back();
		nodeSequences.push_back('N');
		indexToNode.resize(nodeSequences.size(), nodeStart.size()-1);
		nodeEnd.emplace_back(nodeSequences.size());
		notInOrder.push_back(false);
	}
	
	void AddNode(int nodeId, std::string sequence)
	{
		//subgraph extraction might produce different subgraphs with common nodes
		//don't add duplicate nodes
		if (nodeLookup.count(nodeId) != 0) return;

		assert(std::numeric_limits<LengthType>::max() - sequence.size() > nodeSequences.size());
		nodeLookup[nodeId] = nodeStart.size();
		nodeIDs.push_back(nodeId);
		nodeStart.push_back(nodeSequences.size());
		inNeighbors.emplace_back();
		outNeighbors.emplace_back();
		nodeSequences.insert(nodeSequences.end(), sequence.begin(), sequence.end());
		indexToNode.resize(nodeSequences.size(), nodeStart.size()-1);
		nodeEnd.emplace_back(nodeSequences.size());
		notInOrder.push_back(false);
		assert(nodeIDs.size() == nodeStart.size());
		assert(nodeStart.size() == inNeighbors.size());
		assert(inNeighbors.size() == nodeEnd.size());
		assert(nodeEnd.size() == notInOrder.size());
		assert(nodeSequences.size() == indexToNode.size());
		assert(inNeighbors.size() == outNeighbors.size());
	}
	
	void AddEdgeNodeId(int node_id_from, int node_id_to)
	{
		assert(nodeLookup.count(node_id_from) > 0);
		assert(nodeLookup.count(node_id_to) > 0);
		auto from = nodeLookup[node_id_from];
		auto to = nodeLookup[node_id_to];
		assert(to >= 0);
		assert(from >= 0);
		assert(to < inNeighbors.size());
		assert(from < nodeStart.size());

		//subgraph extraction might produce different subgraphs with common edges
		//don't add duplicate edges
		if (std::find(inNeighbors[to].begin(), inNeighbors[to].end(), from) != inNeighbors[to].end()) return;

		inNeighbors[to].push_back(from);
		outNeighbors[from].push_back(to);
		if (from >= to)
		{
			notInOrder[to] = true;
		}
	}

	void Finalize()
	{
		//add the end dummy node as the last node
		dummyNodeEnd = nodeSequences.size();
		nodeIDs.push_back(0);
		nodeStart.push_back(nodeSequences.size());
		inNeighbors.emplace_back();
		outNeighbors.emplace_back();
		nodeSequences.push_back('N');
		indexToNode.resize(nodeSequences.size(), nodeStart.size()-1);
		nodeEnd.emplace_back(nodeSequences.size());
		notInOrder.push_back(false);
		finalized = true;
	}

	AlignmentResult AlignOneWay(const std::string& seq_id, const std::string& sequence, bool reverse, int bandWidth, const std::vector<SeedHit>& seedHits) const
	{
		assert(finalized);
		auto seedHitsInMatrix = getSeedHitPositionsInMatrix(sequence, seedHits);
		auto trace = backtrackWithSquareRootSlices(sequence, bandWidth, seedHitsInMatrix);
		//failed alignment, don't output
		if (std::get<0>(trace) == std::numeric_limits<ScoreType>::min()) return emptyAlignment();
		auto result = traceToAlignment(seq_id, sequence, std::get<0>(trace), std::get<2>(trace), reverse, std::get<1>(trace));
		return result;
	}

	size_t SizeInBp()
	{
		return nodeSequences.size();
	}

private:

	AlignmentResult emptyAlignment() const
	{
		vg::Alignment result;
		result.set_score(std::numeric_limits<decltype(result.score())>::min());
		return AlignmentResult { result, 0, true };
	}

	std::vector<MatrixPosition> getSeedHitPositionsInMatrix(const std::string& sequence, const std::vector<SeedHit>& seedHits) const
	{
		std::vector<MatrixPosition> result;
		for (size_t i = 0; i < seedHits.size(); i++)
		{
			assert(nodeLookup.count(seedHits[i].nodeId) > 0);
			result.emplace_back(nodeStart[nodeLookup.at(seedHits[i].nodeId)] + seedHits[i].nodePos, seedHits[i].sequencePosition);
		}
		return result;
	}

	AlignmentResult traceToAlignment(const std::string& seq_id, const std::string& sequence, ScoreType score, const std::vector<MatrixPosition>& trace, bool reverse, int maxDistanceFromBand) const
	{
		vg::Alignment result;
		result.set_name(seq_id);
		auto path = new vg::Path;
		result.set_allocated_path(path);
		size_t pos = 0;
		size_t oldNode = indexToNode[trace[0].first];
		while (oldNode == dummyNodeStart)
		{
			pos++;
			if (pos == trace.size()) return emptyAlignment();
			assert(pos < trace.size());
			oldNode = indexToNode[trace[pos].first];
			assert(oldNode < nodeIDs.size());
		}
		if (oldNode == dummyNodeEnd) return emptyAlignment();
		int rank = 0;
		auto vgmapping = path->add_mapping();
		auto position = new vg::Position;
		vgmapping->set_allocated_position(position);
		vgmapping->set_rank(rank);
		position->set_node_id(nodeIDs[oldNode]);
		if (reverse) position->set_is_reverse(true);
		for (; pos < trace.size(); pos++)
		{
			if (indexToNode[trace[pos].first] == dummyNodeEnd) break;
			if (indexToNode[trace[pos].first] == oldNode) continue;
			oldNode = indexToNode[trace[pos].first];
			rank++;
			vgmapping = path->add_mapping();
			position = new vg::Position;
			vgmapping->set_allocated_position(position);
			vgmapping->set_rank(rank);
			position->set_node_id(nodeIDs[oldNode]);
			if (reverse) position->set_is_reverse(true);
		}
		result.set_score(score);
		result.set_sequence(sequence);
		return AlignmentResult { result, maxDistanceFromBand, false };
	}

	template <bool distanceMatrixOrder, typename MatrixType>
	std::tuple<ScoreType, int, std::vector<MatrixPosition>> backtrace(const std::vector<ScoreType>& Mslice, const SparseMatrix<MatrixPosition>& backtraceMatrix, const MatrixType& band, int sequenceLength, const Array2D<LengthType, distanceMatrixOrder>& distanceMatrix, const std::vector<MatrixPosition>& seedHits, const SparseMatrix<ScoreType>& scores) const
	{
		auto bandLocations = getBandLocations(sequenceLength, seedHits);
		assert(backtraceMatrix.sizeRows() == sequenceLength+1);
		assert(backtraceMatrix.sizeColumns() == nodeSequences.size());
		std::vector<MatrixPosition> trace;
		bool foundStart = false;
		MatrixPosition currentPosition = std::make_pair(0, sequenceLength);
		//start at the highest value at end of read
		for (size_t i = 0; i < Mslice.size(); i++)
		{
			if (!band(i, sequenceLength)) continue;
			MatrixPosition candidatePosition = std::make_pair(i, sequenceLength);
			if (!foundStart)
			{
				currentPosition = candidatePosition;
				foundStart = true;
			}
			if (Mslice[candidatePosition.first] > Mslice[currentPosition.first])
			{
				currentPosition = candidatePosition;
			}
		}
		assert(band(currentPosition.first, currentPosition.second));
		auto score = Mslice[currentPosition.first];
		trace.push_back(currentPosition);
		LengthType maxMinDistance = 0;
		LengthType maxMinDistanceFromMax = 0;
		while (currentPosition.second > 0)
		{
			LengthType rowMaxScorePosition = -1;
			for (LengthType w = 1; w < dummyNodeEnd; w++)
			{
				if (band(w, currentPosition.second) && (rowMaxScorePosition == -1 || scores.get(w, currentPosition.second) > scores.get(rowMaxScorePosition, currentPosition.second)))
				{
					rowMaxScorePosition = w;
				}
			}
			// std::cerr << rowMaxScorePosition << " ";
			// std::cerr << indexToNode[rowMaxScorePosition] << " ";
			// std::cerr << nodeIDs[indexToNode[rowMaxScorePosition]] << " ";
			// std::cerr << currentPosition.first << " ";
			// std::cerr << indexToNode[currentPosition.first] << " ";
			// std::cerr << nodeIDs[indexToNode[currentPosition.first]] << " ";
			assert(rowMaxScorePosition != -1);
			assert(band(currentPosition.first, currentPosition.second));
			LengthType minDistance = nodeSequences.size();
			for (size_t i = 0; i < bandLocations[currentPosition.second].size(); i++)
			{
				minDistance = std::min(minDistance, distanceFromSeqToSeq(currentPosition.first, bandLocations[currentPosition.second][i], distanceMatrix));
				minDistance = std::min(minDistance, distanceFromSeqToSeq(bandLocations[currentPosition.second][i], currentPosition.first, distanceMatrix));
			}
			LengthType minDistanceFromMax = bandDistanceFromSeqToSeq(currentPosition.first, rowMaxScorePosition, distanceMatrix);
			if (minDistanceFromMax == 104)
			{
				minDistanceFromMax = bandDistanceFromSeqToSeq(currentPosition.first, rowMaxScorePosition, distanceMatrix);
			}
			// std::cerr << distanceFromSeqToSeq(currentPosition.first, rowMaxScorePosition, distanceMatrix) << " ";
			// std::cerr << distanceFromSeqToSeq(rowMaxScorePosition, currentPosition.first, distanceMatrix) << " ";
			// std::cerr << minDistanceFromMax << std::endl;
			if (currentPosition.second > 20)
			{
				maxMinDistanceFromMax = std::max(maxMinDistanceFromMax, minDistanceFromMax);
			}
			maxMinDistance = std::max(maxMinDistance, minDistance);
			assert(currentPosition.second >= 0);
			assert(currentPosition.second < sequenceLength+1);
			assert(currentPosition.first >= 0);
			assert(currentPosition.first < nodeSequences.size());
			//If we're at the dummy node, we have to stay there
			if (currentPosition.first == 0) break;
			auto newPos = backtraceMatrix(currentPosition.first, currentPosition.second);
			assert(newPos.second < currentPosition.second || (newPos.second == currentPosition.second && newPos.first < currentPosition.first));
			currentPosition = newPos;
			trace.push_back(currentPosition);
		}
		std::cerr << "maximum distance from maximum cell: " << maxMinDistanceFromMax << std::endl;
		std::reverse(trace.begin(), trace.end());
		return std::make_tuple(score, maxMinDistance, trace);
	}

	void expandBandForwards(std::vector<std::vector<LengthType>>& result, LengthType w, LengthType j, size_t sequenceLength) const
	{
		if (std::find(result[j].begin(), result[j].end(), w) != result[j].end()) return;
		auto nodeIndex = indexToNode[w];
		auto end = nodeEnd[nodeIndex];
		while (w != end && j < sequenceLength+1)
		{
			result[j].emplace_back(w);
			w++;
			j++;
		}
		if (w == end && j < sequenceLength+1)
		{
			for (size_t i = 0; i < outNeighbors[nodeIndex].size(); i++)
			{
				expandBandForwards(result, nodeStart[outNeighbors[nodeIndex][i]], j, sequenceLength);
			}
		}
	}

	void expandBandBackwards(std::vector<std::vector<LengthType>>& result, LengthType w, LengthType j, size_t sequenceLength) const
	{
		if (std::find(result[j].begin(), result[j].end(), w) != result[j].end()) return;
		auto nodeIndex = indexToNode[w];
		auto start = nodeStart[nodeIndex];
		while (w != start && j > 0)
		{
			result[j].emplace_back(w);
			w--;
			j--;
		}
		if (j > 0) 
		{
			result[j].emplace_back(w);
		}
		if (w == start && j > 0)
		{
			for (size_t i = 0; i < inNeighbors[nodeIndex].size(); i++)
			{
				expandBandBackwards(result, nodeEnd[inNeighbors[nodeIndex][i]] - 1, j-1, sequenceLength);
			}
		}
	}

	std::vector<std::vector<LengthType>> getBandLocations(int sequenceLength, const std::vector<MatrixPosition>& seedHits) const
	{
		std::vector<std::vector<LengthType>> forwardResult;
		std::vector<std::vector<LengthType>> backwardResult;
		backwardResult.resize(sequenceLength+1);
		forwardResult.resize(sequenceLength+1);
		backwardResult[0].emplace_back(0);
		forwardResult[0].emplace_back(0);
		for (auto hit : seedHits)
		{
			expandBandForwards(forwardResult, hit.first, hit.second, sequenceLength);
			expandBandBackwards(backwardResult, hit.first, hit.second, sequenceLength);
		}
		std::vector<std::vector<LengthType>> result;
		result.resize(sequenceLength+1);
		for (size_t j = 0; j < sequenceLength+1; j++)
		{
			std::set<LengthType> rowResult;
			rowResult.insert(forwardResult[j].begin(), forwardResult[j].end());
			rowResult.insert(backwardResult[j].begin(), backwardResult[j].end());
			result[j].insert(result[j].end(), rowResult.begin(), rowResult.end());
		}
		return result;
	}

	template <typename MatrixType>
	std::pair<bool, std::vector<LengthType>> getProcessableColumns(const MatrixType& matrix, LengthType j) const
	{
		std::vector<LengthType> result;
		std::vector<LengthType> inOrder;
		bool hasWrongOrders = false;
		result.reserve(matrix.rowSize(j));
		inOrder.reserve(matrix.rowSize(j));
		for (auto iter = matrix.rowStart(j); iter != matrix.rowEnd(j); ++iter)
		{
			auto w = *iter;
			if (w == dummyNodeStart || w == dummyNodeEnd) continue;
			auto nodeIndex = indexToNode[w];
			if (nodeStart[nodeIndex] == w && notInOrder[nodeIndex])
			{
				result.push_back(w);
				hasWrongOrders = true;
			}
			else
			{
				inOrder.push_back(w);
			}
		}
		std::sort(inOrder.begin(), inOrder.end());
		result.insert(result.end(), inOrder.begin(), inOrder.end());
		return std::make_pair(hasWrongOrders, result);
	}

	template<bool distanceMatrixOrder, typename MatrixType>
	MatrixSlice getScoreAndBacktraceMatrixSlice(const std::string& sequence, bool hasWrongOrders, const Array2D<LengthType, distanceMatrixOrder>& distanceMatrix, MatrixSlice& previous, LengthType start, LengthType end, int bandWidth, const MatrixType& band, SparseMatrix<MatrixPosition>& backtrace) const
	{
		SparseMatrix<ScoreType> scores {nodeSequences.size(), sequence.size()+1};
		std::vector<ScoreType> M1;
		std::vector<ScoreType> M2;
		std::vector<ScoreType> Q1;
		std::vector<ScoreType> Q2;
		std::vector<ScoreType> R1;
		std::vector<ScoreType> R2;
		std::vector<MatrixPosition> Rbacktrace1;
		std::vector<MatrixPosition> Rbacktrace2;
		assert(previous.M.size() == nodeSequences.size());
		assert(previous.R.size() == nodeSequences.size());
		assert(previous.Q.size() == nodeSequences.size());
		assert(previous.Rbacktrace.size() == nodeSequences.size());
		assert(previous.Qbacktrace.size() == nodeSequences.size());
		MatrixSlice result;
		std::vector<MatrixPosition> Qbacktrace;
		M1.resize(nodeSequences.size());
		Q1.resize(nodeSequences.size());
		R1.resize(nodeSequences.size());
		Rbacktrace1.resize(nodeSequences.size());
		std::vector<ScoreType>& currentM = M1;
		std::vector<ScoreType>& previousM = M2;
		std::vector<ScoreType>& currentQ = Q1;
		std::vector<ScoreType>& previousQ = Q2;
		std::vector<ScoreType>& currentR = R1;
		std::vector<ScoreType>& previousR = R2;
		std::vector<MatrixPosition>& currentRbacktrace = Rbacktrace1;
		std::vector<MatrixPosition>& previousRbacktrace = Rbacktrace2;
		previousM = std::move(previous.M);
		previousQ = std::move(previous.Q);
		previousR = std::move(previous.R);
		Qbacktrace = std::move(previous.Qbacktrace);
		previousRbacktrace = std::move(previous.Rbacktrace);
		currentR[dummyNodeStart] = std::numeric_limits<ScoreType>::min() + gapContinuePenalty + 100;
		previousR[dummyNodeStart] = std::numeric_limits<ScoreType>::min() + gapContinuePenalty + 100;
		currentM[dummyNodeStart] = -gapPenalty(start + 1);
		previousM[dummyNodeStart] = -gapPenalty(start);
		currentR[dummyNodeEnd] = std::numeric_limits<ScoreType>::min() + gapContinuePenalty + 100;
		previousR[dummyNodeEnd] = std::numeric_limits<ScoreType>::min() + gapContinuePenalty + 100;
		currentM[dummyNodeEnd] = -gapPenalty(sequence.size() - start - 1);
		previousM[dummyNodeEnd] = -gapPenalty(sequence.size() - start);
		auto previousProcessableColumnsAndOrder = getProcessableColumns(band, start);
		for (LengthType j = 1; j < end - start; j++)
		{
			auto currentProcessableColumnsAndOrder = getProcessableColumns(band, start+j);
			auto& previousProcessableColumns = previousProcessableColumnsAndOrder.second;
			auto& currentProcessableColumns = currentProcessableColumnsAndOrder.second;
			bool hasWrongOrders = currentProcessableColumnsAndOrder.first;
			currentM[dummyNodeStart] = -gapPenalty(start + j);
			currentR[dummyNodeStart] = std::numeric_limits<ScoreType>::min() + gapContinuePenalty + 100;
			backtrace.set(dummyNodeStart, start+j, std::make_pair(dummyNodeStart, start+j-1));
			LengthType maxScorePosition = dummyNodeStart;
			ScoreType maxScore = currentM[dummyNodeStart];
			std::vector<std::pair<LengthType, ScoreType>> Rhelper;
			if (hasWrongOrders) Rhelper = getRHelper(j, start, previousM, sequence, band, previousProcessableColumns);

			for (LengthType w : currentProcessableColumns)
			{
				assert(band(w, j+start));
				bool neighborInsideBand = hasInNeighborInsideBand(w, j, start, band);
				auto nodeIndex = indexToNode[w];
				currentQ[w] = previousQ[w] - gapContinuePenalty;
				bool rCalculated = false;
				if (previousM[w] - gapPenalty(1) > currentQ[w])
				{
					currentQ[w] = previousM[w] - gapPenalty(1);
					Qbacktrace[w] = std::make_pair(w, j-1 + start);
				}
				if (w == nodeStart[nodeIndex] && notInOrder[nodeIndex])
				{
					if (std::any_of(Rhelper.begin(), Rhelper.end(), [w](auto& x) { return x.first != w; }))
					{
						rCalculated = true;
						assert(hasWrongOrders);
						auto rr = fullR(w, j, Rhelper, distanceMatrix, start);
						currentR[w] = rr.first;
						currentRbacktrace[w] = rr.second;
						assert(currentRbacktrace[w].second < (j + start) || (currentRbacktrace[w].second == (j + start) && currentRbacktrace[w].first < w));
					}
				}
				else
				{
					if (neighborInsideBand && previousProcessableColumns.size() > 2)
					{
						rCalculated = true;
						auto rr = recurrenceR(w, j, start, currentM, currentR, currentRbacktrace, band);
						currentR[w] = rr.first;
						currentRbacktrace[w] = rr.second;
						assert(currentRbacktrace[w].second < (j + start) || (currentRbacktrace[w].second == (j + start) && currentRbacktrace[w].first < w));
					}
				}
				//implicitly handle edges from dummy node by initializing M as coming from the dummy node
				currentM[w] = currentM[dummyNodeStart];
				MatrixPosition foundBacktrace = std::make_pair(dummyNodeStart, j);
				if (band(w, start+j-1))
				{
					foundBacktrace = Qbacktrace[w];
					assert(foundBacktrace.second < (j + start) || (foundBacktrace.second == (j + start) && foundBacktrace.first < w));
					currentM[w] = currentQ[w];
				}
				if (rCalculated)
				{
					if (currentR[w] > currentM[w])
					{
						currentM[w] = currentR[w];
						foundBacktrace = currentRbacktrace[w];
						assert(foundBacktrace.second < (j + start) || (foundBacktrace.second == (j + start) && foundBacktrace.first < w));
					}
				}
				if (w == nodeStart[nodeIndex])
				{
					for (size_t i = 0; i < inNeighbors[nodeIndex].size(); i++)
					{
						auto u = nodeEnd[inNeighbors[nodeIndex][i]]-1;
						if (!band(u, start+j-1)) continue;
						//-1 because the rows in the DP matrix are one-based, eg. M[w][1] is the _first_ nucleotide of the read (sequence[0])
						if (previousM[u]+matchScore(nodeSequences[w], sequence[j + start - 1]) > currentM[w])
						{
							currentM[w] = previousM[u]+matchScore(nodeSequences[w], sequence[j + start - 1]);
							foundBacktrace = std::make_pair(u, j-1 + start);
							assert(foundBacktrace.second < (j + start) || (foundBacktrace.second == (j + start) && foundBacktrace.first < w));
						}
					}
				}
				else
				{
					LengthType u = w-1;
					if (band(u, start+j-1))
					{
						//-1 because the rows in the DP matrix are one-based, eg. M[w][1] is the _first_ nucleotide of the read (sequence[0])
						if (previousM[u]+matchScore(nodeSequences[w], sequence[j + start - 1]) > currentM[w])
						{
							currentM[w] = previousM[u]+matchScore(nodeSequences[w], sequence[j + start - 1]);
							foundBacktrace = std::make_pair(u, j-1 + start);
							assert(foundBacktrace.second < (j + start) || (foundBacktrace.second == (j + start) && foundBacktrace.first < w));
						}
					}
				}
				//if the previous row was not inside the band, initialize Q as the current M
				if (!band(w, start+j-1))
				{
					currentQ[w] = currentM[w];
					Qbacktrace[w] = std::make_pair(w, j + start);
				}
				//if R was unavaliable, initialize it as current M
				if (!rCalculated)
				{
					currentR[w] = currentM[w];
					currentRbacktrace[w] = std::make_pair(w, j + start);
				}
				assert(currentM[w] >= -std::numeric_limits<ScoreType>::min() + 100);
				assert(currentM[w] <= std::numeric_limits<ScoreType>::max() - 100);
				scores.set(w, j, currentM[w]);
				backtrace.set(w, j, foundBacktrace);
				assert(foundBacktrace.second < (j + start) || (foundBacktrace.second == (j + start) && foundBacktrace.first < w));
				if (currentM[w] > maxScore)
				{
					maxScore = currentM[w];
					maxScorePosition = w;
				}
			}

			currentM[dummyNodeEnd] = maxScore - gapPenalty(sequence.size() - j);
			backtrace.set(dummyNodeEnd, j, std::make_pair(maxScorePosition, j));

			std::swap(currentM, previousM);
			std::swap(currentQ, previousQ);
			std::swap(currentR, previousR);
			std::swap(currentRbacktrace, previousRbacktrace);
			previousProcessableColumnsAndOrder = std::move(currentProcessableColumnsAndOrder);
		}
		result.Qbacktrace = Qbacktrace;
		//use previous instead of current because the last line swapped them
		result.M = std::move(previousM);
		result.Q = std::move(previousQ);
		result.R = std::move(previousR);
		result.Rbacktrace = std::move(previousRbacktrace);
		result.scores = scores;
		return result;
	}

	template <typename MatrixType>
	void expandBandDownRight(MatrixType& matrix, LengthType w, LengthType j) const
	{
		auto nodeIndex = indexToNode[w];
		auto end = nodeEnd[nodeIndex];
		while (w != end && j < matrix.sizeRows())
		{
			matrix.set(w, j);
			w++;
			j++;
			if (w != end && j < matrix.sizeRows() && matrix(w, j)) return;
		}
		if (j < matrix.sizeRows())
		{
			for (size_t i = 0; i < outNeighbors[nodeIndex].size(); i++)
			{
				expandBandDownRight(matrix, nodeStart[outNeighbors[nodeIndex][i]], j);
			}
		}
	}

	template <typename MatrixType>
	void expandBandRightwards(std::set<MatrixPosition>& diagonallyExpandable, MatrixType& matrix, LengthType w, LengthType j, int bandWidth) const
	{
		auto nodeIndex = indexToNode[w];
		auto end = nodeEnd[nodeIndex];
		while (w != end && bandWidth > 0)
		{
			matrix.set(w, j);
			diagonallyExpandable.emplace(w, j);
			w++;
			bandWidth--;
			if (w != end && matrix(w, j)) return;
		}
		if (w == end && bandWidth > 0)
		{
			for (size_t i = 0; i < outNeighbors[nodeIndex].size(); i++)
			{
				expandBandRightwards(diagonallyExpandable, matrix, nodeStart[outNeighbors[nodeIndex][i]], j, bandWidth);
			}
		}
	}

	template <typename MatrixType>
	void expandBandUpLeft(MatrixType& matrix, LengthType w, LengthType j) const
	{
		if (j == 0)
		{
			matrix.set(w, j);
			return;
		}
		auto nodeIndex = indexToNode[w];
		auto start = nodeStart[nodeIndex];
		while (w != start && j > 0)
		{
			matrix.set(w, j);
			w--;
			j--;
			if (w != start && j > 0 && matrix(w, j)) return;
		}
		matrix.set(w, j);
		if (w == start && j > 0)
		{
			for (size_t i = 0; i < inNeighbors[nodeIndex].size(); i++)
			{
				expandBandUpLeft(matrix, nodeEnd[inNeighbors[nodeIndex][i]] - 1, j-1);
			}
		}
	}

	template <typename MatrixType>
	void expandBandLeftwards(std::set<MatrixPosition>& diagonallyExpandable, MatrixType& matrix, LengthType w, LengthType j, int bandWidth) const
	{
		auto nodeIndex = indexToNode[w];
		auto start = nodeStart[nodeIndex];
		while (w != start && bandWidth > 0)
		{
			matrix.set(w, j);
			diagonallyExpandable.emplace(w, j);
			w--;
			bandWidth--;
			if (w != start && matrix(w, j)) return;
		}
		if (w == start && bandWidth > 0)
		{
			matrix.set(w, j);
			diagonallyExpandable.emplace(w, j);
			for (size_t i = 0; i < inNeighbors[nodeIndex].size(); i++)
			{
				expandBandLeftwards(diagonallyExpandable, matrix, nodeEnd[inNeighbors[nodeIndex][i]] - 1, j, bandWidth-1);
			}
		}
	}

	SparseBoolMatrix<SliceRow<LengthType>> getBandedRows(const std::vector<MatrixPosition>& seedHits, int bandWidth, size_t sequenceLength) const
	{
		SparseBoolMatrix<SliceRow<LengthType>> forward {nodeSequences.size(), sequenceLength+1};
		SparseBoolMatrix<SliceRow<LengthType>> backward {nodeSequences.size(), sequenceLength+1};
		std::set<MatrixPosition> diagonallyExpandable;
		for (auto pos : seedHits)
		{
			forward.set(pos.first, pos.second);
			expandBandRightwards(diagonallyExpandable, forward, pos.first, pos.second, bandWidth);
			expandBandLeftwards(diagonallyExpandable, forward, pos.first, pos.second, bandWidth);
			backward.addRow(pos.second, forward.rowStart(pos.second), forward.rowEnd(pos.second));
		}
		for (auto x : diagonallyExpandable)
		{
			expandBandDownRight(forward, x.first, x.second);
			expandBandUpLeft(backward, x.first, x.second);
		}
		SparseBoolMatrix<SliceRow<LengthType>> result {nodeSequences.size(), sequenceLength+1};
		for (LengthType j = 0; j < sequenceLength+1; j++)
		{
			std::set<LengthType> items;
			items.insert(forward.rowStart(j), forward.rowEnd(j));
			items.insert(backward.rowStart(j), backward.rowEnd(j));
			result.set(dummyNodeStart, j);
			result.addRow(j, items.begin(), items.end());
			result.set(dummyNodeEnd, j);
		}
		return result;
	}

	std::tuple<ScoreType, int, std::vector<MatrixPosition>> backtrackWithSquareRootSlices(const std::string& sequence, int bandWidth, const std::vector<MatrixPosition>& seedHits) const
	{
		auto band = getBandedRows(seedHits, bandWidth, sequence.size());
		auto distanceMatrix = getDistanceMatrixBoostJohnson();
		bool hasWrongOrders = false;
		SparseMatrix<MatrixPosition> backtraceMatrix {nodeSequences.size(), sequence.size() + 1};
		MatrixSlice lastRow = getFirstSlice(bandWidth, backtraceMatrix);
		int sliceSize = sequence.size();
		std::vector<ScoreType> lastRowScore;
		LengthType start = 1;
		//size+1 because the rows in the DP matrix are one-based, eg. M[w][1] is the _first_ nucleotide of the read (sequence[0])
		while (start < sequence.size()+1)
		{
			LengthType end = start + sliceSize;
			if (end > sequence.size()+1) end = sequence.size();
			auto slice = getScoreAndBacktraceMatrixSlice(sequence, hasWrongOrders, distanceMatrix, lastRow, start-1, end, bandWidth, band, backtraceMatrix);
			lastRowScore = slice.M;
			lastRow = std::move(slice);
			start = end;
		}
		auto result = backtrace(lastRow.M, backtraceMatrix, band, sequence.size(), distanceMatrix, seedHits, lastRow.scores);
		return result;
	}

	MatrixSlice getFirstSlice(int bandWidth, SparseMatrix<MatrixPosition>& backtrace) const
	{
		MatrixSlice result;
		result.M.resize(nodeSequences.size(), 0);
		result.R.resize(nodeSequences.size(), 0);
		result.Q.resize(nodeSequences.size(), 0);
		result.Rbacktrace.reserve(nodeSequences.size());
		result.Qbacktrace.reserve(nodeSequences.size());
		for (LengthType i = 0; i < nodeSequences.size(); i++)
		{
			backtrace.set(i, 0, std::make_pair(i, 0));
			result.Qbacktrace.emplace_back(i, 0);
			result.Rbacktrace.emplace_back(i, 0);
		}
		result.R[0] = std::numeric_limits<ScoreType>::min() + gapContinuePenalty + 100;
		assert(result.M.size() == nodeSequences.size());
		assert(result.R.size() == nodeSequences.size());
		assert(result.Q.size() == nodeSequences.size());
		assert(result.Rbacktrace.size() == nodeSequences.size());
		assert(result.Qbacktrace.size() == nodeSequences.size());
		return result;
	}

	std::vector<std::pair<LengthType, ScoreType>> getRHelperZero() const
	{
		std::vector<std::pair<LengthType, ScoreType>> result;
		for (LengthType v = 0; v < nodeSequences.size(); v++)
		{
			result.emplace_back(v, 0);
		}
		return result;
	}

	std::vector<std::pair<LengthType, ScoreType>> getRHelperOne() const
	{
		std::vector<std::pair<LengthType, ScoreType>> result;
		for (LengthType v = 0; v < nodeSequences.size(); v++)
		{
			result.emplace_back(v, 0);
		}
		return result;
	}

	template <typename MatrixType>
	std::vector<std::pair<LengthType, ScoreType>> getRHelper(LengthType j, LengthType start, const std::vector<ScoreType>& previousM, const std::string& sequence, const MatrixType& band, const std::vector<LengthType>& previousProcessableColumns) const
	{
		if (j == 0) return getRHelperZero();
		if (j == 1 && start == 0) return getRHelperOne();
		std::vector<std::tuple<LengthType, ScoreType, ScoreType>> bestPerNode;
		bestPerNode.resize(nodeStart.size(), std::make_tuple(0, std::numeric_limits<ScoreType>::min() + 99, 0));
		for (auto v : previousProcessableColumns)
		{
			auto nodeIndex = indexToNode[v];
			if (nodeStart[nodeIndex] == v)
			{
				for (size_t neighbori = 0; neighbori < inNeighbors[nodeIndex].size(); neighbori++)
				{
					LengthType u = nodeEnd[inNeighbors[nodeIndex][neighbori]]-1;
					if (!band(u, start+j-1)) continue;
					auto scoreHere = previousM[u] + matchScore(nodeSequences[v], sequence[j-1]);
					if (scoreHere - (ScoreType)(nodeEnd[nodeIndex] - v) * gapContinuePenalty > std::get<1>(bestPerNode[nodeIndex]) - std::get<2>(bestPerNode[nodeIndex]))
					{
						bestPerNode[nodeIndex] = std::make_tuple(v, scoreHere, (ScoreType)(nodeEnd[nodeIndex] - v) * gapContinuePenalty);
					}
				}
			}
			else
			{
				LengthType u = v-1;
				if (!band(u, start+j-1)) continue;
				auto scoreHere = previousM[u] + matchScore(nodeSequences[v], sequence[j-1]);
				if (scoreHere - (ScoreType)(nodeEnd[nodeIndex] - v) * gapContinuePenalty > std::get<1>(bestPerNode[nodeIndex]) - std::get<2>(bestPerNode[nodeIndex]))
				{
					bestPerNode[nodeIndex] = std::make_tuple(v, scoreHere, (ScoreType)(nodeEnd[nodeIndex] - v) * gapContinuePenalty);
				}
			}
		}
		std::vector<std::pair<LengthType, ScoreType>> result;
		for (size_t i = 0; i < bestPerNode.size(); i++)
		{
			if (std::get<1>(bestPerNode[i]) > std::numeric_limits<ScoreType>::min() + 100)
			{
				result.emplace_back(std::get<0>(bestPerNode[i]), std::get<1>(bestPerNode[i]));
			}
		}
		assert(result.size() >= 1);
		return result;
	}

	template <typename MatrixType>
	bool hasInNeighborInsideBand(LengthType w, LengthType j, LengthType start, const MatrixType& band) const
	{
		auto nodeIndex = indexToNode[w];
		if (nodeStart[nodeIndex] == w)
		{
			for (size_t neighborI = 0; neighborI < inNeighbors[nodeIndex].size(); neighborI++)
			{
				if (band(nodeEnd[inNeighbors[nodeIndex][neighborI]] - 1, j+start)) return true;
			}
		}
		else
		{
			return band(w-1, start+j);
		}
		return false;
	}

	//compute R using the recurrence on page 3
	template <typename MatrixType>
	std::pair<ScoreType, MatrixPosition> recurrenceR(LengthType w, LengthType j, LengthType start, const std::vector<ScoreType>& currentM, const std::vector<ScoreType>& currentR, const std::vector<MatrixPosition>& currentRbacktrace, const MatrixType& band) const
	{
		assert(band(w, start+j));
		auto nodeIndex = indexToNode[w];
		assert(nodeStart[nodeIndex] != w || !notInOrder[nodeIndex]);
		MatrixPosition pos;
		ScoreType maxValue = std::numeric_limits<ScoreType>::min() + 99;
		if (nodeStart[nodeIndex] == w)
		{
			for (size_t i = 0; i < inNeighbors[nodeIndex].size(); i++)
			{
				auto u = nodeEnd[inNeighbors[nodeIndex][i]]-1;
				if (!band(u, start+j)) continue;
				assert(u < w);
				if (currentM[u] - gapPenalty(1) > maxValue)
				{
					maxValue = currentM[u] - gapPenalty(1);
					pos = std::make_pair(u, j + start);
				}
				if (currentR[u] - gapContinuePenalty > maxValue)
				{
					maxValue = currentR[u] - gapContinuePenalty;
					pos = currentRbacktrace[u];
				}
			}
		}
		else
		{
			auto u = w-1;
			if (band(u, start+j))
			{
				pos = currentRbacktrace[u];
				maxValue = currentR[u] - gapContinuePenalty;
				if (currentM[u] - gapPenalty(1) > maxValue)
				{
					pos = std::make_pair(u, j + start);
					maxValue = currentM[u] - gapPenalty(1);
				}
			}
		}
		assert(maxValue >= -std::numeric_limits<ScoreType>::min() + 100);
		assert(maxValue <= std::numeric_limits<ScoreType>::max() - 100);
		return std::make_pair(maxValue, pos);
	}

	//compute R using the slow, full definition on page 3
	template <bool distanceMatrixOrder>
	std::pair<ScoreType, MatrixPosition> fullR(LengthType w, LengthType j, const std::vector<std::pair<LengthType, ScoreType>>& RHelper, const Array2D<LengthType, distanceMatrixOrder>& distanceMatrix, LengthType start) const
	{
		assert(j > 0);
		assert(w > 0);
		auto nodeIndex = indexToNode[w];
		assert(nodeStart[nodeIndex] == w && notInOrder[nodeIndex]);
		MatrixPosition pos;
		ScoreType maxValue = std::numeric_limits<ScoreType>::min() + 99;
		for (auto pair : RHelper)
		{
			auto v = pair.first;
			if (v == w) continue;
			auto scoreHere = pair.second - gapPenalty(distanceFromSeqToSeq(v, w, distanceMatrix));
			if (scoreHere > maxValue)
			{
				maxValue = scoreHere;
				pos = std::make_pair(v, j-1 + start);
			}
		}
		assert(maxValue >= -std::numeric_limits<ScoreType>::min() + 100);
		assert(maxValue <= std::numeric_limits<ScoreType>::max() - 100);
		return std::make_pair(maxValue, pos);
	}

	template <bool distanceMatrixOrder>
	LengthType bandDistanceFromSeqToSeq(LengthType start, LengthType end, const Array2D<LengthType, distanceMatrixOrder>& distanceMatrix) const
	{
		if (start == end) return 0;
		if (start == dummyNodeStart || start == dummyNodeEnd || end == dummyNodeStart || end == dummyNodeEnd) return 1;
		auto startNode = indexToNode[start];
		auto endNode = indexToNode[end];
		if (startNode == endNode) return std::min(end - start, start - end);
		if (distanceMatrix(startNode, endNode) == nodeEnd[startNode]-nodeStart[startNode])
		{
			return nodeEnd[startNode] - start + end - nodeStart[endNode];
		}
		if (distanceMatrix(endNode, startNode) == nodeEnd[endNode]-nodeStart[endNode])
		{
			return nodeEnd[endNode] - end + start - nodeStart[startNode];
		}
		LengthType minDistance = nodeSequences.size();
		for (size_t i = 0; i < distanceMatrix.sizeRows(); i++)
		{
			LengthType distanceFromStartToMid = distanceMatrix(startNode, i) + nodeStart[startNode] - start;
			if (distanceMatrix(i, startNode) + start - nodeStart[startNode] < distanceFromStartToMid)
			{
				distanceFromStartToMid = distanceMatrix(i, startNode) + start - nodeStart[startNode];
			}
			LengthType distanceFromMidToEnd = distanceMatrix(i, endNode) + end - nodeStart[endNode];
			if (distanceMatrix(endNode, i) + nodeStart[endNode] - end < distanceFromMidToEnd)
			{
				distanceFromMidToEnd = distanceMatrix(endNode, i) + nodeStart[endNode] - end;
			}
			minDistance = std::min(minDistance, distanceFromStartToMid + distanceFromMidToEnd);

			LengthType distanceFromStartToMidEnd = distanceMatrix(startNode, i) + nodeStart[startNode] - start + nodeEnd[i] - nodeStart[i];
			if (distanceMatrix(i, startNode) - (nodeEnd[i] - nodeStart[i]) + start - nodeStart[startNode] < distanceFromStartToMidEnd)
			{
				distanceFromStartToMidEnd = distanceMatrix(i, startNode) - (nodeEnd[i] - nodeStart[i]) + start - nodeStart[startNode];
			}
			LengthType distanceFromMidEndToEnd = distanceMatrix(i, endNode) - (nodeEnd[i] - nodeStart[i]) + end - nodeStart[endNode];
			if (distanceMatrix(endNode, i) + (nodeEnd[i] - nodeStart[i]) + nodeStart[endNode] - end < distanceFromMidToEnd)
			{
				distanceFromMidToEnd = distanceMatrix(endNode, i) + (nodeEnd[i] - nodeStart[i]) + nodeStart[endNode] - end;
			}
			minDistance = std::min(minDistance, distanceFromStartToMidEnd + distanceFromMidEndToEnd);
		}
		return minDistance;
	}

	template <bool distanceMatrixOrder>
	LengthType distanceFromSeqToSeq(LengthType start, LengthType end, const Array2D<LengthType, distanceMatrixOrder>& distanceMatrix) const
	{
		if (start == end) return 0;
		if (start == dummyNodeStart || start == dummyNodeEnd || end == dummyNodeStart || end == dummyNodeEnd) return 1;
		auto startNode = indexToNode[start];
		auto endNode = indexToNode[end];
		if (startNode == endNode && end >= start) return end - start;
		return distanceMatrix(startNode, endNode) + nodeStart[startNode] + end - nodeStart[endNode] - start;
	}

	Array2D<LengthType, false> getDistanceMatrixBoostJohnson() const
	{
		//http://www.boost.org/doc/libs/1_40_0/libs/graph/example/johnson-eg.cpp
		auto V = inNeighbors.size();
		adjacency_list<vecS, vecS, directedS, no_property, property<edge_weight_t, int, property<edge_weight2_t, int>>> graph { inNeighbors.size() };
		for (size_t i = 0; i < inNeighbors.size(); i++)
		{
			for (size_t j = 0; j < inNeighbors[i].size(); j++)
			{
				boost::add_edge(inNeighbors[i][j], i, graph);
			}
		}
		property_map<adjacency_list<vecS, vecS, directedS, no_property, property<edge_weight_t, int, property<edge_weight2_t, int>>>, edge_weight_t>::type w = get(edge_weight, graph);
		graph_traits<adjacency_list<vecS, vecS, directedS, no_property, property<edge_weight_t, int, property<edge_weight2_t, int>>>>::edge_iterator e, e_end;
		for (boost::tie(e, e_end) = edges(graph); e != e_end; ++e)
		{
			auto startIndex = (*e).m_source;
			w[*e] = nodeEnd[startIndex] - nodeStart[startIndex];
		}
		std::vector<int> d(V, nodeSequences.size()+1);
		int** D;
		D = new int*[inNeighbors.size()];
		for (size_t i = 0; i < inNeighbors.size(); i++)
		{
			D[i] = new int[inNeighbors.size()];
			for (size_t j = 0; j < inNeighbors.size(); j++)
			{
				D[i][j] = nodeSequences.size()+1;
			}
		}
		johnson_all_pairs_shortest_paths(graph, D, distance_map(&d[0]));
		Array2D<LengthType, false> result {inNeighbors.size(), inNeighbors.size(), std::numeric_limits<LengthType>::max()};
		for (size_t i = 0; i < inNeighbors.size(); i++)
		{
			for (size_t j = 0; j < inNeighbors.size(); j++)
			{
				if (D[i][j] == std::numeric_limits<int>::max())
				{
					result(i, j) = nodeSequences.size()+1;
				}
				else
				{
					result(i, j) = D[i][j];
				}
			}
		}
		for (size_t i = 0; i < inNeighbors.size(); i++)
		{
			delete [] D[i];
		}
		delete [] D;
		//make sure that the distance to itself is not 0
		//we need to do this so distance calculation from a later point in the node to an earlier point in the node works correctly
		for (size_t i = 0; i < inNeighbors.size(); i++)
		{
			result(i, i) = nodeSequences.size()+1;
			for (size_t j = 0; j < inNeighbors.size(); j++)
			{
				if (j == i) continue;
				result(i, i) = std::min(result(i, i), result(i, j) + result(j, i));
			}
		}
		return result;
	}

	ScoreType gapPenalty(LengthType length) const
	{
		if (length == 0) return 0;
		return gapStartPenalty + gapContinuePenalty * (length - 1);
	}

	ScoreType matchScore(char graph, char sequence) const
	{
		return graph == sequence ? 1 : -1;
	}

	std::vector<bool> notInOrder;
	std::vector<LengthType> nodeStart;
	std::vector<LengthType> nodeEnd;
	std::vector<LengthType> indexToNode;
	std::map<int, LengthType> nodeLookup;
	std::vector<int> nodeIDs;
	std::vector<std::vector<LengthType>> inNeighbors;
	std::vector<std::vector<LengthType>> outNeighbors;
	std::string nodeSequences;
	ScoreType gapStartPenalty;
	ScoreType gapContinuePenalty;
	LengthType dummyNodeStart = 0;
	LengthType dummyNodeEnd = 1;
	bool finalized;
};

#endif