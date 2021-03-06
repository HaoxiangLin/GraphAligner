#include <vector>
#include <algorithm>
#include <iostream>
#include <tuple>
#include "CommonUtils.h"
#include "GfaGraph.h"


namespace std 
{
	template <> 
	struct hash<std::pair<size_t, size_t>>
	{
		size_t operator()(const std::pair<size_t, size_t>& x) const
		{
			return hash<size_t>()(x.first) ^ hash<size_t>()(x.second);
		}
	};
}

struct Node
{
	int nodeId;
	bool reverse;
	bool operator==(const Node& other) const
	{
		return nodeId == other.nodeId && reverse == other.reverse;
	}
};

struct Alignment
{
	std::vector<Node> path;
	std::string name;
};

Alignment convertVGtoAlignment(const vg::Alignment& vgAln)
{
	Alignment result;
	result.name = vgAln.name();
	for (int i = 0; i < vgAln.path().mapping_size(); i++)
	{
		result.path.emplace_back();
		result.path.back().nodeId = vgAln.path().mapping(i).position().node_id();
		result.path.back().reverse = vgAln.path().mapping(i).position().is_reverse();
	}
	return result;
}

Alignment reverse(const Alignment& old)
{
	Alignment result;
	result.name = old.name;
	for (size_t i = 0; i < old.path.size(); i++)
	{
		result.path.emplace_back();
		result.path.back().nodeId = old.path[i].nodeId;
		result.path.back().reverse = !old.path[i].reverse;
	}
	std::reverse(result.path.begin(), result.path.end());
	return result;
}

double getAlignmentIdentity(const Alignment& read, const Alignment& transcript, const std::vector<size_t>& nodeLengths)
{
	std::vector<std::vector<size_t>> matchLen;
	matchLen.resize(read.path.size()+1);
	for (size_t i = 0; i < read.path.size()+1; i++)
	{
		matchLen[i].resize(transcript.path.size()+1, 0);
	}
	for (size_t i = 0; i < read.path.size(); i++)
	{
		for (size_t j = 0; j < transcript.path.size(); j++)
		{
			matchLen[i+1][j+1] = std::max(matchLen[i+1][j], matchLen[i][j+1]);
			if (read.path[i] == transcript.path[j])
			{
				matchLen[i+1][j+1] = std::max(matchLen[i+1][j+1], matchLen[i][j] + nodeLengths[read.path[i].nodeId]);
			}
			else
			{
				matchLen[i+1][j+1] = std::max(matchLen[i+1][j+1], matchLen[i][j]);
			}
		}
	}
	size_t maxMatch = 0;
	for (size_t j = 0; j < transcript.path.size(); j++)
	{
		maxMatch = std::max(maxMatch, matchLen.back()[j+1]);
	}
	assert(maxMatch >= 0);
	size_t readLen = 0;
	for (size_t i = 0; i < read.path.size(); i++)
	{
		readLen += nodeLengths[read.path[i].nodeId];
	}
	assert(maxMatch <= readLen);
	return (double)maxMatch / (double)readLen;
}

int main(int argc, char** argv)
{
	std::string transcriptFile { argv[1] };
	std::string readFile { argv[2] };
	std::string graphFile { argv[3] };

	std::vector<size_t> nodeLengths;
	{
		if (graphFile.substr(graphFile.size() - 4) == ".gfa")
		{
			auto graph = GfaGraph::LoadFromFile(graphFile);
			nodeLengths.resize(graph.nodes.size()+1, 0);
			for (auto node : graph.nodes)
			{
				nodeLengths[node.first] = node.second.size();
			}
		}
		else if (graphFile.substr(graphFile.size() - 3) == ".vg")
		{
			auto graph = CommonUtils::LoadVGGraph(graphFile);
			nodeLengths.resize(graph.node_size()+1, 0);
			for (int i = 0; i < graph.node_size(); i++)
			{
				nodeLengths[graph.node(i).id()] = graph.node(i).sequence().size();
			}
		}
		else
		{
			assert(false);
		}
	}

	std::vector<Alignment> transcripts;
	std::vector<Alignment> reads;
	{
		auto vgtranscripts = CommonUtils::LoadVGAlignments(transcriptFile);
		for (auto vg : vgtranscripts)
		{
			transcripts.push_back(convertVGtoAlignment(vg));
		}
	}
	{
		auto vgreads = CommonUtils::LoadVGAlignments(readFile);
		for (auto vg : vgreads)
		{
			reads.push_back(convertVGtoAlignment(vg));
		}
	}

	std::unordered_map<int, std::vector<size_t>> transcriptsCrossingNode;
	for (size_t i = 0; i < transcripts.size(); i++)
	{
		for (int j = 0; j < transcripts[i].path.size(); j++)
		{
			transcriptsCrossingNode[transcripts[i].path[j].nodeId].push_back(i);
		}
	}

	std::unordered_map<std::pair<size_t, size_t>, double> readTranscriptBestPair;

	for (size_t readi = 0; readi < reads.size(); readi++)
	{
		auto read = reads[readi];
		std::set<size_t> possibleTranscripts;
		for (size_t i = 0; i < read.path.size(); i++)
		{
			possibleTranscripts.insert(transcriptsCrossingNode[read.path[i].nodeId].begin(), transcriptsCrossingNode[read.path[i].nodeId].end());
		}
		auto reverseread = reverse(read);
		for (auto i : possibleTranscripts)
		{
			auto identityFw = getAlignmentIdentity(read, transcripts[i], nodeLengths);
			auto identityBw = getAlignmentIdentity(reverseread, transcripts[i], nodeLengths);
			auto bigger = std::max(identityFw, identityBw);
			if (bigger > 0 && (readTranscriptBestPair.count(std::make_pair(readi, i)) == 0 || readTranscriptBestPair[std::make_pair(readi, i)] < bigger))
			{
				readTranscriptBestPair[std::make_pair(readi, i)] = bigger;
			}
		}
	}
	for (auto mapping : readTranscriptBestPair)
	{
		std::cout << reads[mapping.first.first].name << "\t" << transcripts[mapping.first.second].name << "\t" << mapping.second << std::endl;
	}
}
