//***************************************************************************
//* Copyright (c) 2023-2024 SPAdes team
//* Copyright (c) 2015-2022 Saint Petersburg State University
//* Copyright (c) 2011-2014 Saint Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************
#pragma once

#include "gzstream/gzstream.h"
#include "annotation.hpp"
#include "io/reads/io_helper.hpp"
#include "io/reads/osequencestream.hpp"
#include "pipeline/sequence_mapper_gp_api.hpp"

namespace debruijn_graph {

class ContigBinner {
    const graph_pack::GraphPack& gp_;
    const EdgeAnnotation& edge_annotation_;
    std::filesystem::path out_root_;
    std::filesystem::path sample_name_;
    std::shared_ptr<SequenceMapper<Graph>> mapper_;
    std::set<std::string> bins_of_interest_;

    typedef io::OPairedReadStream<ogzstream, io::FastqWriter> Stream;
    std::map<bin_id, std::unique_ptr<Stream>> out_streams_;

    std::set<bin_id> RelevantBins(const io::SingleRead& r) const;

    void Init(bin_id bin);

public:
    ContigBinner(const graph_pack::GraphPack& gp,
                 const EdgeAnnotation& edge_annotation,
                 const std::string& out_root,
                 const std::string& sample_name,
                 const std::vector<std::string>& bins_of_interest = {}) :
                     gp_(gp),
                     edge_annotation_(edge_annotation),
                     out_root_(out_root),
                     sample_name_(sample_name),
                     mapper_(MapperInstance(gp)),
                     bins_of_interest_(bins_of_interest.begin(), bins_of_interest.end()) {
    }

    ~ContigBinner() {
        out_streams_.clear();
    }

    void Run(io::PairedStream& paired_reads);
};

void BinReads(const graph_pack::GraphPack& gp, const std::string& out_root,
             const std::string& sample,
             const std::string& left_reads, const std::string& right_reads,
             const EdgeAnnotation& edge_annotation,
             const std::vector<std::string>& bins_of_interest);

}
