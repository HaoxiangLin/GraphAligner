#include <iostream>
#include <unistd.h>
#include <fstream>
#include "Aligner.h"
#include "stream.hpp"

int main(int argc, char** argv)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	std::string graphFile = "";
	std::string fastqFile = "";
	std::string seedFile = "";
	std::string alignmentFile = "";
	std::string auggraphFile = "";
	int numThreads = 0;
	int c;

	while ((c = getopt(argc, argv, "g:f:s:a:t:A:")) != -1)
	{
		switch(c)
		{
			case 'g':
				graphFile = std::string(optarg);
				break;
			case 'f':
				fastqFile = std::string(optarg);
				break;
			case 's':
				seedFile = std::string(optarg);
				break;
			case 'a':
				alignmentFile = std::string(optarg);
				break;
			case 't':
				numThreads = std::stoi(optarg);
				break;
			case 'A':
				auggraphFile = std::string(optarg);
				break;
		}
	}

	if (numThreads < 1)
	{
		std::cerr << "number of threads must be >= 1" << std::endl;
		std::exit(0);
	}

	alignReads(graphFile, fastqFile, seedFile, numThreads, alignmentFile, auggraphFile);

	return 0;
}
