//***************************************************************************
//* Copyright (c) 2018 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "gfa_writer.hpp"

#include "assembly_graph/core/graph.hpp"
#include "assembly_graph/core/graph_iterators.hpp"
#include "assembly_graph/components/graph_component.hpp"

using namespace gfa;
using namespace debruijn_graph;

template class omnigraph::GraphComponent<Graph>;

static void WriteSegment(const std::string& edge_id, const Sequence &seq,
                         double cov, uint64_t kmers,
                         std::ostream &os) {
    os << "S\t"
       << edge_id << '\t' << seq.str() << '\t'
       << "DP:f:" << float(cov) << '\t'
       << "KC:i:" << kmers << '\n';
}

static void WriteLink(EdgeId e1, EdgeId e2, size_t overlap_size,
                      std::ostream &os, io::CanonicalEdgeHelper<Graph> &namer) {
    os << "L\t"
       << namer.EdgeOrientationString(e1, "\t") << '\t'
       << namer.EdgeOrientationString(e2, "\t") << '\t'
       << overlap_size << "M\n";
}

void GFAWriter::WriteSegments() {
    for (EdgeId e : graph_.canonical_edges()) {
        WriteSegment(edge_namer_.EdgeString(e), graph_.EdgeNucls(e),
                     graph_.coverage(e), graph_.kmer_multiplicity(e),
                     os_);
    }
}

void GFAWriter::WriteLinks() {
    for (VertexId v : graph_.canonical_vertices()) {
        for (auto inc_edge : graph_.IncomingEdges(v)) {
            for (auto out_edge : graph_.OutgoingEdges(v)) {
                WriteLink(inc_edge, out_edge, graph_.length(v),
                          os_, edge_namer_);
            }
        }
    }
}


void GFAWriter::WriteSegments(const Component &gc) {
    for (EdgeId e : gc.edges()) {
        if (e <= graph_.conjugate(e)) {
            WriteSegment(edge_namer_.EdgeString(e), graph_.EdgeNucls(e),
                         graph_.coverage(e), graph_.kmer_multiplicity(e),
                         os_);
        }
    }
}

void GFAWriter::WriteLinks(const Component &gc) {
    for (VertexId v : gc.vertices()) {
        if (v <= graph_.conjugate(v)) {
            for (auto inc_edge : graph_.IncomingEdges(v)) {
                if (!gc.contains(inc_edge))
                    continue;

                for (auto out_edge : graph_.OutgoingEdges(v)) {
                    if (!gc.contains(out_edge))
                        continue;

                    WriteLink(inc_edge, out_edge, graph_.k(),
                              os_, edge_namer_);
                }
            }
        }
    }
}

void GFAWriter::WriteSegmentsAndLinks(const Component &gc) {
    WriteSegments(gc);
    WriteLinks(gc);
}

