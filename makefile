CC=gcc
GPP=g++
CPPFLAGS=-Wall -std=c++14 -O3 -g

ODIR=obj
BINDIR=bin

LIBS=-lm -lprotobuf -lz -lboost_serialization

DEPS = vg.pb.h fastqloader.h GraphAlignerWrapper.h vg.pb.h BigraphToDigraph.h stream.hpp Aligner.h ThreadReadAssertion.h AlignmentGraph.h CommonUtils.h GfaGraph.h AlignmentCorrectnessEstimation.h OrderedIndexKeeper.h UniqueQueue.h

_OBJ = Aligner.o AlignerMain.o vg.pb.o fastqloader.o BigraphToDigraph.o ThreadReadAssertion.o AlignmentGraph.o CommonUtils.o GraphAlignerWrapper.o GfaGraph.o AlignmentCorrectnessEstimation.o
OBJ = $(patsubst %, $(ODIR)/%, $(_OBJ))

$(ODIR)/GraphAlignerWrapper.o: GraphAlignerWrapper.cpp GraphAligner.h $(DEPS)

$(ODIR)/%.o: %.cpp $(DEPS)
	$(GPP) -c -o $@ $< $(CPPFLAGS)

$(BINDIR)/Aligner: $(OBJ)
	$(GPP) -o $@ $^ $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed -lpthread -pthread -static-libstdc++

$(BINDIR)/ReadIndexToId: $(OBJ)
	$(GPP) -o $@ ReadIndexToId.cpp $(ODIR)/CommonUtils.o $(ODIR)/vg.pb.o $(ODIR)/fastqloader.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/CompareAlignments: $(OBJ)
	$(GPP) -o $@ CompareAlignments.cpp $(ODIR)/CommonUtils.o $(ODIR)/vg.pb.o $(ODIR)/fastqloader.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/MergeGraphs: $(OBJ)
	$(GPP) -o $@ MergeGraphs.cpp $(ODIR)/CommonUtils.o $(ODIR)/vg.pb.o $(ODIR)/fastqloader.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/SimulateReads: $(OBJ)
	$(GPP) -o $@ SimulateReads.cpp $(ODIR)/CommonUtils.o $(ODIR)/vg.pb.o $(ODIR)/fastqloader.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/ReverseReads: $(OBJ)
	$(GPP) -o $@ ReverseReads.cpp $(ODIR)/CommonUtils.o $(ODIR)/vg.pb.o $(ODIR)/fastqloader.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/PickSeedHits: $(OBJ)
	$(GPP) -o $@ PickSeedHits.cpp $(ODIR)/CommonUtils.o $(ODIR)/vg.pb.o $(ODIR)/fastqloader.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed -static-libstdc++

$(BINDIR)/AlignmentSequenceInserter: $(OBJ)
	$(GPP) -o $@ AlignmentSequenceInserter.cpp $(ODIR)/CommonUtils.o $(ODIR)/fastqloader.o $(ODIR)/vg.pb.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/SupportedSubgraph: $(OBJ)
	$(GPP) -o $@ SupportedSubgraph.cpp $(ODIR)/CommonUtils.o $(ODIR)/ThreadReadAssertion.o $(ODIR)/fastqloader.o $(ODIR)/vg.pb.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/MafToAlignment: $(OBJ)
	$(GPP) -o $@ MafToAlignment.cpp $(ODIR)/CommonUtils.o $(ODIR)/ThreadReadAssertion.o $(ODIR)/fastqloader.o $(ODIR)/vg.pb.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/ExtractPathSequence: $(OBJ)
	$(GPP) -o $@ ExtractPathSequence.cpp $(ODIR)/CommonUtils.o $(ODIR)/GfaGraph.o $(ODIR)/ThreadReadAssertion.o $(ODIR)/fastqloader.o $(ODIR)/vg.pb.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed -static-libstdc++

$(BINDIR)/AlignmentOverlap: $(OBJ)
	$(GPP) -o $@ AlignmentOverlap.cpp $(ODIR)/CommonUtils.o $(ODIR)/ThreadReadAssertion.o $(ODIR)/fastqloader.o $(ODIR)/vg.pb.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed

$(BINDIR)/Bluntify: $(OBJ)
	$(GPP) -o $@ Bluntify.cpp $(ODIR)/CommonUtils.o $(ODIR)/ThreadReadAssertion.o $(ODIR)/fastqloader.o $(ODIR)/vg.pb.o $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed -static-libstdc++

$(BINDIR)/BluntifyEdgeGroup: $(OBJ)
	$(GPP) -o $@ BluntifyEdgeGroup.cpp $(CPPFLAGS) -Wl,-Bstatic $(LIBS) -Wl,-Bdynamic -Wl,--as-needed -static-libstdc++

all: $(BINDIR)/Aligner $(BINDIR)/ReadIndexToId $(BINDIR)/CompareAlignments $(BINDIR)/SimulateReads $(BINDIR)/ReverseReads $(BINDIR)/PickSeedHits $(BINDIR)/AlignmentSequenceInserter $(BINDIR)/MergeGraphs $(BINDIR)/SupportedSubgraph $(BINDIR)/MafToAlignment $(BINDIR)/ExtractPathSequence $(BINDIR)/AlignmentOverlap $(BINDIR)/Bluntify

clean:
	rm -f $(ODIR)/*
	rm -f $(BINDIR)/*
