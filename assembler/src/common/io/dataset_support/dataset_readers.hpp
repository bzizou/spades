//***************************************************************************
//* Copyright (c) 2015 Saint Petersburg State University
//* Copyright (c) 2011-2014 Saint Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#pragma once

#include "io/reads/io_helper.hpp"
#include "pipeline/library.hpp"

namespace io {

inline
PairedStream paired_easy_reader(const SequencingLibraryBase &lib,
                                bool followed_by_rc,
                                size_t insert_size,
                                bool use_orientation = true,
                                OffsetType offset_type = PhredOffset,
                                ThreadPool::ThreadPool *pool = nullptr) {
    ReadStreamList<PairedRead> streams;
    for (const auto &read_pair : lib.paired_reads()) {
        streams.push_back(PairedEasyStream(read_pair.first, read_pair.second, followed_by_rc, insert_size,
                                           use_orientation, lib.orientation(), offset_type, pool));
    }
    return MultifileWrap<PairedRead>(std::move(streams));
}

inline
ReadStreamList<SingleRead> single_easy_readers(const SequencingLibraryBase &lib,
                                               bool followed_by_rc,
                                               bool including_paired_reads,
                                               bool handle_Ns = true,
                                               OffsetType offset_type = PhredOffset,
                                               ThreadPool::ThreadPool *pool = nullptr) {
    ReadStreamList<SingleRead> streams;
    if (including_paired_reads) {
      for (const auto &read : lib.reads()) {
        //do we need input_file function here?
        streams.push_back(EasyStream(read, followed_by_rc, handle_Ns, offset_type, pool));
      }
    } else {
      for (const auto &read : lib.single_reads()) {
        streams.push_back(EasyStream(read, followed_by_rc, handle_Ns, offset_type, pool));
      }
    }
    return streams;
}

inline
SingleStream single_easy_reader(const SequencingLibraryBase &lib,
                                bool followed_by_rc,
                                bool including_paired_reads,
                                bool handle_Ns = true,
                                OffsetType offset_type = PhredOffset,
                                ThreadPool::ThreadPool *pool = nullptr) {
    return MultifileWrap<io::SingleRead>(
        single_easy_readers(lib, followed_by_rc, including_paired_reads, handle_Ns, offset_type, pool));
}

inline
ReadStreamList<SingleRead> merged_easy_readers(const SequencingLibraryBase &lib,
                                               bool followed_by_rc,
                                               bool handle_Ns = true,
                                               OffsetType offset_type = PhredOffset,
                                               ThreadPool::ThreadPool *pool = nullptr) {
    ReadStreamList<SingleRead> streams;
    for (const auto& read : lib.merged_reads()) {
        streams.push_back(EasyStream(read, followed_by_rc, handle_Ns, offset_type, pool));
    }
    return streams;
}

inline
SingleStream merged_easy_reader(const SequencingLibraryBase &lib,
                                bool followed_by_rc,
                                bool handle_Ns = true,
                                OffsetType offset_type = PhredOffset,
                                ThreadPool::ThreadPool *pool = nullptr) {
    return MultifileWrap<io::SingleRead>(
        merged_easy_readers(lib, followed_by_rc, handle_Ns, offset_type, pool));
}

}
