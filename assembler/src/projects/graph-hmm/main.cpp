//***************************************************************************
//* Copyright (c) 2018 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "assembly_graph/core/graph.hpp"
#include "assembly_graph/dijkstra/dijkstra_helper.hpp"
#include "assembly_graph/components/graph_component.hpp"
#include "assembly_graph/paths/bidirectional_path_io/bidirectional_path_output.hpp"

#include "visualization/visualization.hpp"
#include "pipeline/graphio.hpp"
#include "io/graph/gfa_reader.hpp"
#include "io/reads/io_helper.hpp"
#include "io/reads/osequencestream.hpp"

#include "utils/parallel/openmp_wrapper.h"
#include "utils/logger/log_writers.hpp"
#include "utils/segfault_handler.hpp"

#include "version.hpp"

#include "hmmfile.hpp"
#include "hmmmatcher.hpp"
#include "fees.hpp"
#include "omnigraph_wrapper.hpp"

#include <clipp/clipp.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string>

#include "aa.hpp"
#include "depth_filter.hpp"

void create_console_logger() {
    using namespace logging;

    logger *lg = create_logger("");
    lg->add_writer(std::make_shared<mutex_writer>(std::make_shared<console_writer>()));
    attach_logger(lg);
}

template <typename T>
std::string join(const std::vector<T> &v, const std::string &sep = "_") {
    std::stringstream ss;
    for (size_t i = 0; i < v.size(); ++i) {
        ss << v[i];
        if (i != v.size() - 1) ss << sep;
    }

    return ss.str();
}

struct cfg {
    std::string load_from;
    std::string hmmfile;
    std::string output_dir = "";
    size_t k;
    int threads = 4;
    size_t top;
    uint64_t int_id;
    unsigned max_size;
    bool debug;
    bool draw;
    bool save;
    bool rescore;
    bool annotate_graph;

    hmmer::hmmer_cfg hcfg;
    cfg()
            : load_from(""), hmmfile(""), k(0), top(10),
              int_id(0), max_size(1000),
              debug(false), draw(false),
              save(true), rescore(true), annotate_graph(true)
    {}
};

extern "C" {
#include "p7_config.h"

#include <stdlib.h>
#include <string.h>

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_sq.h"

#include "hmmer.h"
}

void process_cmdline(int argc, char **argv, cfg &cfg) {
  using namespace clipp;

  auto cli = (
      cfg.hmmfile    << value("hmm file"),
      cfg.load_from  << value("load from"),
      cfg.k          << integer("k-mer size"),
      required("--output", "-o") & value("output directory", cfg.output_dir)    % "output directory",
      (option("--top") & integer("x", cfg.top)) % "extract top x paths",
      (option("--threads", "-t") & integer("value", cfg.threads)) % "number of threads",
      (option("--edge_id") & integer("value", cfg.int_id)) % "match around edge",
      (option("--max_size") & integer("value", cfg.max_size)) % "maximal component size to consider (default: 1000)",
      // Control of output
      cfg.hcfg.acc     << option("--acc")          % "prefer accessions over names in output",
      cfg.hcfg.noali   << option("--noali")        % "don't output alignments, so output is smaller",
      // Control of reporting thresholds
      (option("-E") & number("value", cfg.hcfg.E))        % "report sequences <= this E-value threshold in output",
      (option("-T") & number("value", cfg.hcfg.T))        % "report sequences >= this score threshold in output",
      (option("--domE") & number("value", cfg.hcfg.domE)) % "report domains <= this E-value threshold in output",
      (option("--domT") & number("value", cfg.hcfg.domT)) % "report domains >= this score cutoff in output",
      // Control of inclusion (significance) thresholds
      (option("-incE") & number("value", cfg.hcfg.incE))       % "consider sequences <= this E-value threshold as significant",
      (option("-incT") & number("value", cfg.hcfg.incT))       % "consider sequences >= this score threshold as significant",
      (option("-incdomE") & number("value", cfg.hcfg.incdomE)) % "consider domains <= this E-value threshold as significant",
      (option("-incdomT") & number("value", cfg.hcfg.incdomT)) % "consider domains >= this score threshold as significant",
      // Model-specific thresholding for both reporting and inclusion
      cfg.hcfg.cut_ga  << option("--cut_ga")       % "use profile's GA gathering cutoffs to set all thresholding",
      cfg.hcfg.cut_nc  << option("--cut_nc")       % "use profile's NC noise cutoffs to set all thresholding",
      cfg.hcfg.cut_tc  << option("--cut_tc")       % "use profile's TC trusted cutoffs to set all thresholding",
      // Control of acceleration pipeline
      cfg.hcfg.max     << option("--max")             % "Turn all heuristic filters off (less speed, more power)",
      (option("--F1") & number("value", cfg.hcfg.F1)) % "Stage 1 (MSV) threshold: promote hits w/ P <= F1",
      (option("--F2") & number("value", cfg.hcfg.F2)) % "Stage 2 (Vit) threshold: promote hits w/ P <= F2",
      (option("--F3") & number("value", cfg.hcfg.F3)) % "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",
      cfg.debug << option("--debug") % "enable extensive debug output",
      cfg.draw  << option("--draw")  % "draw pictures around the interesting edges",
      cfg.save << option("--save") % "save found sequences",
      cfg.rescore  << option("--rescore")  % "rescore paths via HMMer",
      cfg.annotate_graph << option("--annotate-graph") % "emit paths in GFA graph"
  );

  if (!parse(argc, argv, cli)) {
    std::cout << make_man_page(cli, argv[0]);
    exit(1);
  }
}

void DrawComponent(const omnigraph::GraphComponent<debruijn_graph::ConjugateDeBruijnGraph> &component,
                   const debruijn_graph::ConjugateDeBruijnGraph &graph,
                   const std::string &prefix,
                   const std::vector<debruijn_graph::EdgeId> &match_edges) {
    using namespace visualization;
    using namespace visualization::visualization_utils;
    using namespace debruijn_graph;

    // FIXME: This madness needs to be refactored
    graph_labeler::StrGraphLabeler<ConjugateDeBruijnGraph> tmp_labeler1(graph);
    graph_labeler::CoverageGraphLabeler<ConjugateDeBruijnGraph> tmp_labeler2(graph);
    graph_labeler::CompositeLabeler<ConjugateDeBruijnGraph> labeler{tmp_labeler1, tmp_labeler2};

    auto colorer = graph_colorer::DefaultColorer(graph);
    auto edge_colorer = std::make_shared<graph_colorer::CompositeEdgeColorer<ConjugateDeBruijnGraph>>("black");
    edge_colorer->AddColorer(colorer);
    edge_colorer->AddColorer(std::make_shared<graph_colorer::SetColorer<ConjugateDeBruijnGraph>>(graph, match_edges, "green"));
    std::shared_ptr<graph_colorer::GraphColorer<ConjugateDeBruijnGraph>>
            resulting_colorer = std::make_shared<graph_colorer::CompositeGraphColorer<Graph>>(colorer, edge_colorer);

    WriteComponent(component,
                   prefix + ".dot",
                   resulting_colorer,
                   labeler);
}

template<class GraphCursor>
std::vector<typename GraphCursor::EdgeId> to_path(const std::vector<GraphCursor> &cpath) {
    std::vector<typename GraphCursor::EdgeId> path;

    auto it = cpath.begin();
    for (; it != cpath.end() && it->is_empty(); ++it) {}

    for (; it != cpath.end(); ++it) {
        if (it->is_empty())
            continue;

        if (path.size() == 0 || it->edge() != path.back()) {
            path.push_back(it->edge());
        } else {
            for (auto e : it->edges())
                if (e != path.back())
                    path.push_back(e);
        }

    }

    return path;
}

using debruijn_graph::EdgeId;
using debruijn_graph::VertexId;
using debruijn_graph::ConjugateDeBruijnGraph;
using EdgeAlnInfo = std::unordered_map<EdgeId, std::pair<int, int>>;


auto ScoreSequences(const std::vector<std::string> &seqs,
                    const std::vector<std::string> &refs,
                    const hmmer::HMM &hmm, const cfg &cfg) {
    bool hmm_in_aas = hmm.abc()->K == 20;
    hmmer::HMMMatcher matcher(hmm, cfg.hcfg);

    if (!hmm_in_aas) {
        INFO("HMM in nucleotides");
    } else {
        INFO("HMM in amino acids");
    }

    for (size_t i = 0; i < seqs.size(); ++i) {
        std::string ref = refs.size() > i ? refs[i] : std::to_string(i);
        if (!hmm_in_aas) {
            matcher.match(ref.c_str(), seqs[i].c_str());
        } else {
            for (size_t shift = 0; shift < 3; ++shift) {
                std::string ref_shift = ref + "/" + std::to_string(shift);
                std::string seq_aas = translate(seqs[i].c_str() + shift);
                matcher.match(ref_shift.c_str(), seq_aas.c_str());
            }
        }
    }

    matcher.summarize();
    return matcher;
}

EdgeAlnInfo get_matched_edges(const std::vector<EdgeId> &edges,
                              const hmmer::HMMMatcher &matcher,
                              const cfg &cfg) {
    EdgeAlnInfo match_edges;
    for (const auto &hit : matcher.hits()) {
        if (!hit.reported() || !hit.included())
            continue;

        EdgeId e = edges[std::stoull(hit.name())];
        if (cfg.debug)
            INFO("HMMER seq id:" <<  hit.name() << ", edge id:" << e);

        for (const auto &domain : hit.domains()) {
            // Calculate HMM overhang
            std::pair<int, int> seqpos = domain.seqpos();
            std::pair<int, int> hmmpos = domain.hmmpos();

            int roverhang = static_cast<int>(domain.M() - hmmpos.second) - static_cast<int>(domain.L() - seqpos.second);
            int loverhang = static_cast<int>(hmmpos.first) - static_cast<int>(seqpos.first);

            if (!match_edges.count(e)) {
                match_edges[e] = {loverhang, roverhang};
            } else {
                auto &entry = match_edges[e];
                if (entry.first < loverhang) {
                    entry.first = loverhang;
                }
                if (entry.second < roverhang) {
                    entry.second = roverhang;
                }
            }

            INFO("" << e << ":" << match_edges[e]);
        }
    }
    INFO("Total matched edges: " << match_edges.size());

    return match_edges;
}

EdgeAlnInfo MatchedEdges(const std::vector<EdgeId> &edges,
                         const ConjugateDeBruijnGraph &graph,
                         const hmmer::HMM &hmm, const cfg &cfg) {
    std::vector<std::string> seqs;
    for (size_t i = 0; i < edges.size(); ++i) {
        seqs.push_back(graph.EdgeNucls(edges[i]).str());
    }
    auto matcher = ScoreSequences(seqs, {}, hmm, cfg);

    auto match_edges = get_matched_edges(edges, matcher, cfg);

    if (match_edges.size() && cfg.debug) {
        int textw = 120;
        #pragma omp critical(console)
        {
            p7_tophits_Targets(stdout, matcher.top_hits(), matcher.pipeline(), textw); if (fprintf(stderr, "\n\n") < 0) FATAL_ERROR("write failed");
            p7_tophits_Domains(stdout, matcher.top_hits(), matcher.pipeline(), textw); if (fprintf(stderr, "\n\n") < 0) FATAL_ERROR("write failed");
            p7_pli_Statistics(stdout, matcher.pipeline(), nullptr); if (fprintf(stderr, "//\n") < 0) FATAL_ERROR("write failed");
        }
    }

    return match_edges;
}

void OutputMatches(const hmmer::HMM &hmm, const hmmer::HMMMatcher &matcher, const std::string &filename,
                   const std::string &format = "tblout") {
    P7_HMM *p7hmm = hmm.get();
    FILE *fp = fopen(filename.c_str(), "w");
    if (format == "domtblout") {
        p7_tophits_TabularDomains(fp, p7hmm->name, p7hmm->acc, matcher.top_hits(), matcher.pipeline(), true);
        // TODO Output tail
    } else if (format == "tblout") {
        p7_tophits_TabularTargets(fp, p7hmm->name, p7hmm->acc, matcher.top_hits(), matcher.pipeline(), true);
        // TODO Output tail
    } else if (format == "pfamtblout") {
        p7_tophits_TabularXfam(fp, p7hmm->name, p7hmm->acc, matcher.top_hits(), matcher.pipeline());
    } else {
        FATAL_ERROR("unknown output format");
    }
    fclose(fp);
}

std::string PathToString(const std::vector<EdgeId>& path,
                         const ConjugateDeBruijnGraph &graph) {
    std::string res = "";
    for (auto e : path)
        res = res + graph.EdgeNucls(e).First(graph.length(e)).str();
    return res;
}

template<class Graph>
Sequence MergeSequences(const Graph &g,
                        const std::vector<typename Graph::EdgeId> &continuous_path) {
    std::vector<Sequence> path_sequences;
    path_sequences.push_back(g.EdgeNucls(continuous_path[0]));
    for (size_t i = 1; i < continuous_path.size(); ++i) {
        VERIFY(g.EdgeEnd(continuous_path[i - 1]) == g.EdgeStart(continuous_path[i]));
        path_sequences.push_back(g.EdgeNucls(continuous_path[i]));
    }
    return MergeOverlappingSequences(path_sequences, g.k());
}

static bool ends_with(const std::string &s, const std::string &p) {
    if (s.size() < p.size())
        return false;

    return (s.compare(s.size() - p.size(), p.size(), p) == 0);
}

std::unordered_map<EdgeId, std::unordered_set<VertexId>> ExtractNeighbourhoods(const EdgeAlnInfo &matched_edges,
                                                                               const ConjugateDeBruijnGraph &graph,
                                                                               int mult) {
    using namespace debruijn_graph;

    std::unordered_map<EdgeId, std::unordered_set<VertexId>> neighbourhoods;
    for (const auto &entry : matched_edges) {
        EdgeId e = entry.first;
        INFO("Extracting neighbourhood of edge " << e);

        std::pair<int, int> overhangs = entry.second;
        overhangs.first *= mult; overhangs.second *= mult;
        INFO("Dijkstra bounds set to " << overhangs);

        std::vector<VertexId> fvertices, bvertices;
        // If hmm overhangs from the edge, then run edge-bounded dijkstra to
        // extract the graph neighbourhood.
        if (overhangs.second > 0) {
            auto fdijkstra = omnigraph::CreateEdgeBoundedDijkstra(graph, overhangs.second);
            fdijkstra.Run(graph.EdgeEnd(e));
            fvertices = fdijkstra.ReachedVertices();
        }
        if (overhangs.first > 0) {
            auto bdijkstra = omnigraph::CreateBackwardEdgeBoundedDijkstra(graph, overhangs.first);
            bdijkstra.Run(graph.EdgeStart(e));
            bvertices = bdijkstra.ReachedVertices();
        }

        INFO("Total " << std::make_pair(bvertices.size(), fvertices.size()) << " extracted");

        neighbourhoods[e].insert(fvertices.begin(), fvertices.end());
        neighbourhoods[e].insert(bvertices.begin(), bvertices.end());
        neighbourhoods[e].insert(graph.EdgeEnd(e));
        neighbourhoods[e].insert(graph.EdgeStart(e));
    }

    return neighbourhoods;
}

using Neighbourhoods = std::unordered_map<EdgeId, std::unordered_set<VertexId>>;

Neighbourhoods join_components(const Neighbourhoods &neighbourhoods,
                               const EdgeAlnInfo &matched_edges,
                               const debruijn_graph::ConjugateDeBruijnGraph &graph) {
    std::vector<std::pair<EdgeId, std::unordered_set<VertexId>>> neighbourhoods_vector(neighbourhoods.cbegin(),
                                                                                       neighbourhoods.cend());
    auto unmatched_part_length = [&matched_edges, &graph](EdgeId edge_id) {
        std::pair<int, int> overhangs = matched_edges.find(edge_id)->second;
        return std::max(0, overhangs.first) + std::max(0, overhangs.second);
    };

    std::sort(neighbourhoods_vector.begin(), neighbourhoods_vector.end(),
              [&unmatched_part_length](const auto &n1, const auto &n2) {
                  return unmatched_part_length(n1.first) < unmatched_part_length(n2.first);
              });

    std::unordered_set<EdgeId> removed;
    for (auto it = neighbourhoods_vector.begin(); it != neighbourhoods_vector.end(); ++it) {
        if (removed.count(it->first)) {
            continue;
        }
        for (auto to_check = std::next(it); to_check != neighbourhoods_vector.end(); ++to_check) {
            VertexId vstart = graph.EdgeStart(to_check->first), vend = graph.EdgeEnd(to_check->first);
            if (it->second.count(vstart) || it->second.count(vend)) {
                it->second.insert(to_check->second.begin(), to_check->second.end());
                removed.insert(to_check->first);
            }
        }
    }

    Neighbourhoods result;
    for (auto it = neighbourhoods_vector.begin(); it != neighbourhoods_vector.end(); ++it) {
        if (!removed.count(it->first)) {
            result.insert(*it);
        }
    }

    return result;
}

std::vector<hmmer::HMM> ParseHMMFile(const std::string &filename) {
    /* Open the query profile HMM file */
    hmmer::HMMFile hmmfile(filename);
    if (!hmmfile.valid()) {
        FATAL_ERROR("Error opening HMM file " << filename);
    }

    std::vector<hmmer::HMM> hmms;

    while (auto hmmw = hmmfile.read())
        hmms.emplace_back(std::move(hmmw.get()));

    if (hmms.empty()) {
        FATAL_ERROR("Error reading HMM file " << filename);
    }

    return hmms;
}

template <typename Container>
auto EdgesToSequences(const Container &entries,
                      const debruijn_graph::ConjugateDeBruijnGraph &graph) {
    std::vector<std::pair<std::string, std::string>> ids_n_seqs;
    for (const auto &entry : entries) {
        std::string id = join(entry);
        std::string seq = MergeSequences(graph, entry).str();
        ids_n_seqs.push_back({id, seq});
    }

    std::sort(ids_n_seqs.begin(), ids_n_seqs.end());
    return ids_n_seqs;
}

template <typename Container>
void ExportEdges(const Container &entries,
                 const debruijn_graph::ConjugateDeBruijnGraph &graph,
                 const std::string &filename) {
    if (entries.size() == 0)
        return;

    std::ofstream o(filename, std::ios::out);

    for (const auto &kv : EdgesToSequences(entries, graph)) {
        o << ">" << kv.first << "\n";
        io::WriteWrapped(kv.second, o);
    }
}

void LoadGraph(debruijn_graph::ConjugateDeBruijnGraph &graph, const std::string &filename) {
    using namespace debruijn_graph;
    if (ends_with(filename, ".gfa")) {
        gfa::GFAReader gfa(filename);
        INFO("GFA segments: " << gfa.num_edges() << ", links: " << gfa.num_links());
        gfa.to_graph(graph);
    } else {
        graphio::ScanBasicGraph(filename, graph);
    }
}

struct HMMPathInfo {
    std::string hmmname;
    EdgeId leader;
    unsigned priority;
    double score;
    std::string seq;
    std::vector<EdgeId> path;
    std::string alignment;

    bool operator<(const HMMPathInfo &that) const {
        return score < that.score;
    }

    HMMPathInfo(std::string name, EdgeId e, unsigned prio, double sc, std::string s, std::vector<EdgeId> p, std::string alignment)
            : hmmname(std::move(name)), leader(e), priority(prio), score(sc),
              seq(std::move(s)), path(std::move(p)), alignment(std::move(alignment)) {}
};

void SaveResults(const hmmer::HMM &hmm, const ConjugateDeBruijnGraph &graph,
                 const cfg &cfg,
                 const std::vector<HMMPathInfo> &results) {
    P7_HMM *p7hmm = hmm.get();

    INFO("Total " << results.size() << " resultant paths extracted");

    std::unordered_set<std::vector<EdgeId>> to_rescore_local;
    if (cfg.save) {
        if (results.size()) {
            {
                std::ofstream o(cfg.output_dir + std::string("/") + p7hmm->name + ".seqs.fa", std::ios::out);
                for (const auto &result : results) {
                    if (result.seq.size() == 0)
                        continue;
                    o << ">Score=" << result.score << "|Edges=" << join(result.path) << "|Alignment=" << result.alignment << '\n';
                    io::WriteWrapped(result.seq, o);
                }
            }

            {
                std::ofstream o(cfg.output_dir + std::string("/") + p7hmm->name + ".fa", std::ios::out);
                for (const auto &result : results) {
                    o << ">" << result.leader << "_" << result.priority;
                    if (result.seq.size() == 0)
                        o << " (whole edge)";
                    o << '\n';
                    if (result.seq.size() == 0) {
                        io::WriteWrapped(graph.EdgeNucls(result.leader).str(), o);
                    } else {
                        io::WriteWrapped(result.seq, o);
                    }
                    if (cfg.rescore && result.path.size() > 0) {
                        to_rescore_local.insert(result.path);
                    }
                }
            }
        }
    }
}

void Rescore(const hmmer::HMM &hmm, const ConjugateDeBruijnGraph &graph,
             const cfg &cfg,
             const std::vector<HMMPathInfo> &results) {
    P7_HMM *p7hmm = hmm.get();

    std::unordered_set<std::vector<EdgeId>> to_rescore;

    for (const auto &result : results) {
        if (result.path.size() == 0)
            continue;

        to_rescore.insert(result.path);
    }

    INFO("Total " << to_rescore.size() << " local paths to rescore");

    ExportEdges(to_rescore, graph, cfg.output_dir + std::string("/") + p7hmm->name + ".edges.fa");

    std::vector<std::string> seqs_to_rescore;
    std::vector<std::string> refs_to_rescore;
    for (const auto &kv : EdgesToSequences(to_rescore, graph)) {
        refs_to_rescore.push_back(kv.first);
        seqs_to_rescore.push_back(kv.second);
    }

    auto matcher = ScoreSequences(seqs_to_rescore, refs_to_rescore, hmm, cfg);
    OutputMatches(hmm, matcher, cfg.output_dir + "/" + p7hmm->name + ".tblout", "tblout");
    OutputMatches(hmm, matcher, cfg.output_dir + "/" + p7hmm->name + ".domtblout", "domtblout");
    OutputMatches(hmm, matcher, cfg.output_dir + "/" + p7hmm->name + ".pfamtblout", "pfamtblout");
}

void TraceHMM(const hmmer::HMM &hmm,
              const debruijn_graph::ConjugateDeBruijnGraph &graph, const std::vector<EdgeId> &edges,
              const cfg &cfg,
              std::vector<HMMPathInfo> &results) {
    P7_HMM *p7hmm = hmm.get();

    if (fprintf(stdout, "Query:       %s  [M=%d]\n", p7hmm->name, p7hmm->M) < 0) FATAL_ERROR("write failed");
    if (p7hmm->acc)  { if (fprintf(stdout, "Accession:   %s\n", p7hmm->acc)  < 0) FATAL_ERROR("write failed"); }
    if (p7hmm->desc) { if (fprintf(stdout, "Description: %s\n", p7hmm->desc) < 0) FATAL_ERROR("write failed"); }

    // Collect the neighbourhood of the matched edges
    EdgeAlnInfo matched_edges = MatchedEdges(edges, graph, hmm, cfg);  // hhmer is thread-safe
    bool hmm_in_aas = hmm.abc()->K == 20;
    Neighbourhoods neighbourhoods = ExtractNeighbourhoods(matched_edges, graph, (hmm_in_aas ? 6 : 2));

    // See, whether we could join some components
    INFO("Joining components");
    neighbourhoods = join_components(neighbourhoods, matched_edges, graph);
    INFO("Total unique neighbourhoods extracted " << neighbourhoods.size());

    auto fees = hmm::fees_from_hmm(p7hmm, hmm.abc());

    auto run_search = [&](const auto &initial, EdgeId e, size_t top,
                          std::vector<HMMPathInfo> &local_results) {
        auto result = find_best_path(fees, initial);

        INFO("Best score: " << result.best_score());
        INFO("Extracting top paths");
        auto top_paths = result.top_k(top);
        if (!top_paths.empty()) {
          INFO("Best of the best");
          INFO(top_paths.str(0));
        }
        size_t idx = 0;
        for (const auto& annotated_path : top_paths) {
            auto seq = top_paths.str(annotated_path.path);
            auto alignment = top_paths.event_str(annotated_path);
            auto edge_path = to_path(annotated_path.path);
            if (edge_path.size() == 0)
                continue;

            local_results.emplace_back(p7hmm->name, e, idx++, annotated_path.score, seq, std::move(edge_path), std::move(alignment));
        }
    };

    std::vector<EdgeId> match_edges;
    for (const auto &entry : matched_edges)
        match_edges.push_back(entry.first);

    for (const auto &kv : neighbourhoods) {
        EdgeId e = kv.first;

        INFO("Looking HMM path around " << e);
        auto component = omnigraph::GraphComponent<ConjugateDeBruijnGraph>::FromVertices(graph,
                                                                                         kv.second.begin(), kv.second.end(),
                                                                                         true);
        INFO("Neighbourhood vertices: " << component.v_size() << ", edges: " << component.e_size());

        if (component.e_size()/2 > cfg.max_size) {
            WARN("Component is too large (" << component.e_size() / 2 << " vs " << cfg.max_size << "), skipping");
            continue;
        }

        if (cfg.draw) {
            INFO("Writing component around edge " << e);
            DrawComponent(component, graph, std::to_string(graph.int_id(e)), match_edges);
        }

        int coef = hmm_in_aas ? 3 : 1;
        int loverhang = (matched_edges[e].first + 10) * coef; // TODO unify overhangs processing
        int roverhang = (matched_edges[e].second + 10) * coef;

        using GraphCursor = std::decay_t<decltype(all(component)[0])>;
        std::vector<GraphCursor> neib_cursors;
        if (loverhang > 0) {
            GraphCursor start = get_cursor(component, e, 0);
            auto left_cursors = impl::depth_subset(start, loverhang * 2, false);
            neib_cursors.insert(neib_cursors.end(), left_cursors.cbegin(), left_cursors.cend());
        }

        size_t len = component.g().length(e) + component.g().k();
        INFO("Edge length: " << len);
        INFO("Edge overhangs: " << loverhang << " " << roverhang);
        if (roverhang > 0) {
            GraphCursor end = get_cursor(component, e, len - 1);
            auto right_cursors = impl::depth_subset(end, roverhang * 2, true);
            neib_cursors.insert(neib_cursors.end(), right_cursors.cbegin(), right_cursors.cend());
        }

        for (size_t i = std::max(0, -loverhang); i < len - std::max(0, -roverhang); ++i) {
            neib_cursors.push_back(get_cursor(component, e, i));
        }

        // neib_cursors = all(component);

        INFO("Running path search");
        std::vector<HMMPathInfo> local_results;

        if (hmm_in_aas) {
            run_search(make_aa_cursors(neib_cursors), e, cfg.top, local_results);
        } else {
            run_search(neib_cursors, e, cfg.top, local_results);
        }

        results.insert(results.end(), local_results.begin(), local_results.end());

        std::unordered_set<std::vector<EdgeId>> paths;
        for (const auto& entry : local_results)
            paths.insert(entry.path);
        INFO("Total " << paths.size() << " unique edge paths extracted");

        {
            size_t idx = 0;
            for (const auto &path : paths) {
                INFO("Path length : " << path.size() << " edges");
                for (EdgeId e : path)
                    INFO("" << e.int_id());
                if (cfg.draw) {
                    INFO("Writing component around path");
                    DrawComponent(component, graph, std::to_string(graph.int_id(e)) + "_" + std::to_string(idx), path);
                }
                idx += 1;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    utils::segfault_handler sh;
    utils::perf_counter pc;

    srand(42);
    srandom(42);

    cfg cfg;
    process_cmdline(argc, argv, cfg);

    create_console_logger();

    int status = mkdir(cfg.output_dir.c_str(), 0775);
    if (status != 0) {
        if (errno == EEXIST) {
            WARN("Output directory exists: " << cfg.output_dir);
        } else {
            ERROR("Cannot create output directory: " << cfg.output_dir);
            std::exit(1);
        }
    }

    INFO("Starting Graph HMM aligning engine, built from " SPADES_GIT_REFSPEC ", git revision " SPADES_GIT_SHA1);

    using namespace debruijn_graph;

    debruijn_graph::ConjugateDeBruijnGraph graph(cfg.k);
    LoadGraph(graph, cfg.load_from);
    INFO("Graph loaded. Total vertices: " << graph.size());

    // Collect all the edges
    std::vector<EdgeId> edges;
    for (auto it = graph.ConstEdgeBegin(); !it.IsEnd(); ++it) {
        EdgeId edge = *it;
        if (cfg.int_id == 0 || edge.int_id() == cfg.int_id) edges.push_back(edge);
    }

    std::unordered_set<std::vector<EdgeId>> to_rescore;
    std::set<std::pair<std::string, std::vector<EdgeId>>> gfa_paths;

    // Outer loop: over each query HMM in <hmmfile>.
    auto hmms = ParseHMMFile(cfg.hmmfile);
    omp_set_num_threads(cfg.threads);
    #pragma omp parallel for
    for (size_t _i = 0; _i < hmms.size(); ++_i) {
        const auto &hmm = hmms[_i];

        std::vector<HMMPathInfo> results;

        TraceHMM(hmm, graph, edges, cfg, results);

        std::sort(results.begin(), results.end());
        SaveResults(hmm, graph, cfg, results);

        if (cfg.annotate_graph) {
            std::unordered_set<std::vector<EdgeId>> unique_paths;
            for (const auto &result : results)
                unique_paths.insert(result.path);

            size_t idx = 0;
            for (const auto &path : unique_paths) {
                #pragma omp critical
                {
                    gfa_paths.insert({ std::string(hmm.get()->name) + "_" + std::to_string(idx++), path });
                }
            }
        }

        if (cfg.rescore) {
            Rescore(hmm, graph, cfg, results);

            for (const auto &result : results) {
                #pragma omp critical
                {
                    to_rescore.insert(result.path);
                }
            }
        }
    } // end outer loop over query HMMs

    if (cfg.rescore) {
        INFO("Total " << to_rescore.size() << " paths to rescore");
        ExportEdges(to_rescore, graph, cfg.output_dir + "/all.edges.fa");
    }

    if (cfg.annotate_graph) {
        std::string fname = cfg.output_dir + "/graph_with_hmm_paths.gfa";
        INFO("Saving annotated graph to " << fname)
        std::ofstream os(fname);
        path_extend::GFAPathWriter gfa_writer(graph, os);
        gfa_writer.WriteSegmentsAndLinks();

        for (const auto& entry : gfa_paths) {
            gfa_writer.WritePaths(entry.second, entry.first);
        }
    }

    return 0;
}
