//
// Created by Davide  Caroselli on 28/09/16.
//

#include "SuffixArray.h"
#include "dbkv.h"
#include <rocksdb/slice_transform.h>
#include <rocksdb/merge_operator.h>
#include <boost/filesystem.hpp>
#include <thread>
#include <fstream>
#include <iostream>

namespace fs = boost::filesystem;

using namespace rocksdb;
using namespace mmt;
using namespace mmt::sapt;

static const string kStreamsKey = MakeEmptyKey(kStreamsKeyType);
static const string kStorageManifestKey = MakeEmptyKey(kStorageManifestKeyType);

/*
 * MergePositionOperator
 */

namespace mmt {
    namespace sapt {

        class MergePositionOperator : public AssociativeMergeOperator {
        public:
            virtual bool Merge(const Slice &key, const Slice *existing_value, const Slice &value, string *new_value,
                               Logger *logger) const override {
                switch (key.data_[0]) {
                    case kSourcePrefixKeyType:
                        MergePositionLists(existing_value, value, new_value);
                        return true;
                    case kTargetCountKeyType:
                        MergeCounts(existing_value, value, new_value);
                        return true;
                    default:
                        return false;
                }
            }

            inline void MergePositionLists(const Slice *existing_value, const Slice &value, string *new_value) const {
                if (existing_value)
                    *new_value = existing_value->ToString() + value.ToString();
                else
                    *new_value = value.ToString();
            }

            inline void MergeCounts(const Slice *existing_value, const Slice &value, string *new_value) const {
                int64_t count = DeserializeCount(value.data(), value.size());
                if (existing_value)
                    count += DeserializeCount(existing_value->data(), existing_value->size());

                *new_value = SerializeCount(count);
            }

            virtual const char *Name() const override {
                return "MergePositionOperator";
            }
        };

    }
}

/*
 * SuffixArray - Initialization
 */

SuffixArray::SuffixArray(const string &modelPath, uint8_t prefixLength, double gcTimeout, size_t gcBatchSize,
                         bool prepareForBulkLoad) throw(index_exception, storage_exception) :
        openForBulkLoad(prepareForBulkLoad), prefixLength(prefixLength) {
    fs::path modelDir(modelPath);

    if (!fs::is_directory(modelDir))
        throw invalid_argument("Invalid model path: " + modelPath);

    fs::path storageFolder = fs::absolute(modelDir / fs::path("storage"));
    fs::path indexPath = fs::absolute(modelDir / fs::path("index"));

    rocksdb::Options options;
    options.create_if_missing = true;
    options.merge_operator.reset(new MergePositionOperator);
    options.max_open_files = -1;
    options.compaction_style = kCompactionStyleLevel;

    if (prepareForBulkLoad) {
        options.PrepareForBulkLoad();
    } else {
        unsigned cpus = thread::hardware_concurrency();

        if (cpus > 1)
            options.IncreaseParallelism(cpus > 4 ? 4 : 2);

        options.level0_file_num_compaction_trigger = 8;
        options.level0_slowdown_writes_trigger = 17;
        options.level0_stop_writes_trigger = 24;
        options.num_levels = 4;

        options.write_buffer_size = 64L * 1024L * 1024L;
        options.max_write_buffer_number = 3;
        options.target_file_size_base = 64L * 1024L * 1024L;
        options.max_bytes_for_level_base = 512L * 1024L * 1024L;
        options.max_bytes_for_level_multiplier = 8;
    }

    Status status = DB::Open(options, indexPath.string(), &db);
    if (!status.ok())
        throw index_exception(status.ToString());

    // Read streams
    string raw_streams;

    db->Get(ReadOptions(), kStreamsKey, &raw_streams);
    DeserializeStreams(raw_streams.data(), raw_streams.size(), &streams);

    // Load storage
    string raw_manifest;

    db->Get(ReadOptions(), kStorageManifestKey, &raw_manifest);
    StorageManifest *manifest = StorageManifest::Deserialize(raw_manifest.data(), raw_manifest.size());

    storage = new CorporaStorage(storageFolder.string(), manifest);

    // Garbage collector
    garbageCollector = new GarbageCollector(storage, db, prefixLength, gcBatchSize, gcTimeout);
}

SuffixArray::~SuffixArray() {
    delete garbageCollector;
    delete db;
    delete storage;
}

/*
 * SuffixArray - Indexing
 */

void SuffixArray::ForceCompaction() throw(index_exception) {
    if (openForBulkLoad) {
        WriteBatch writeBatch;

        // Write streams
        writeBatch.Put(kStreamsKey, SerializeStreams(streams));

        // Write storage manifest
        storage->Flush();
        string manifest = storage->GetManifest()->Serialize();
        writeBatch.Put(kStorageManifestKey, manifest);

        // Commit write batch
        Status status = db->Write(WriteOptions(), &writeBatch);
        if (!status.ok())
            throw index_exception("Unable to write to index: " + status.ToString());
    }

    db->CompactRange(CompactRangeOptions(), NULL, NULL);
}

void SuffixArray::PutBatch(UpdateBatch &batch) throw(index_exception, storage_exception) {
    WriteBatch writeBatch;

    // Compute prefixes
    unordered_map<string, PostingList> sourcePrefixes;
    unordered_map<string, int64_t> targetCounts;

    for (auto entry = batch.data.begin(); entry != batch.data.end(); ++entry) {
        domain_t domain = entry->domain;

        int64_t offset = storage->Append(domain, entry->source, entry->target, entry->alignment);
        AddPrefixesToBatch(domain, entry->source, offset, sourcePrefixes);
        AddTargetCountsToBatch(entry->target, targetCounts);
    }

    // Add prefixes to write batch
    for (auto prefix = sourcePrefixes.begin(); prefix != sourcePrefixes.end(); ++prefix) {
        string value = prefix->second.Serialize();
        writeBatch.Merge(prefix->first, value);
    }

    // Add target counts to write batch
    for (auto count = targetCounts.begin(); count != targetCounts.end(); ++count) {
        string value = SerializeCount(count->second);
        writeBatch.Merge(count->first, value);
    }

    // Write deleted domains
    for (auto domain = batch.deletions.begin(); domain != batch.deletions.end(); ++domain)
        writeBatch.Put(MakeDomainDeletionKey(*domain), "");

    // Write streams
    writeBatch.Put(kStreamsKey, SerializeStreams(streams));

    // Write storage manifest
    if (!openForBulkLoad) {
        storage->Flush();
        string manifest = storage->GetManifest()->Serialize();
        writeBatch.Put(kStorageManifestKey, manifest);
    }

    // Commit write batch
    Status status = db->Write(WriteOptions(), &writeBatch);
    if (!status.ok())
        throw index_exception("Unable to write to index: " + status.ToString());

    // Reset streams and domains
    streams = batch.GetStreams();
    garbageCollector->MarkForDeletion(batch.deletions);
}

void SuffixArray::AddPrefixesToBatch(domain_t domain, const vector<wid_t> &sentence,
                                     int64_t location, unordered_map<string, PostingList> &outBatch) {
    size_t size = sentence.size();

    for (size_t start = 0; start < size; ++start) {
        for (size_t length = 1; length <= prefixLength; ++length) {
            if (start + length > size)
                break;

            string dkey = MakePrefixKey(prefixLength, domain, sentence, start, length);
            outBatch[dkey].Append(domain, location, (length_t) start);
        }
    }
}

void SuffixArray::AddTargetCountsToBatch(const vector<wid_t> &sentence, unordered_map<string, int64_t> &outBatch) {
    size_t size = sentence.size();

    for (size_t start = 0; start < size; ++start) {
        for (size_t length = 1; length <= prefixLength; ++length) {
            if (start + length > size)
                break;

            string dkey = MakeCountKey(prefixLength, sentence, start, length);
            outBatch[dkey]++;
        }
    }
}

/*
 * SuffixArray - Query
 */

size_t SuffixArray::CountOccurrences(bool isSource, const vector<wid_t> &phrase) {
    if (phrase.size() > prefixLength)
        return 1; // Approximate higher order n-grams to singletons

    int64_t count = 0;

    if (isSource) {
        PrefixCursor *cursor = PrefixCursor::NewGlobalCursor(db, prefixLength);
        for (cursor->Seek(phrase); cursor->HasNext(); cursor->Next())
            count += cursor->CountValue();
        delete cursor;
    } else {
        string key = MakeCountKey(prefixLength, phrase, 0, phrase.size());
        string value;

        db->Get(ReadOptions(), key, &value);
        count = DeserializeCount(value.data(), value.size());
    }

    return (size_t) std::max(count, (int64_t) 1);
}

void SuffixArray::GetRandomSamples(const vector<wid_t> &phrase, size_t limit, vector<sample_t> &outSamples,
                                   const context_t *context, bool searchInBackground) {
    Collector collector(storage, db, prefixLength, context, searchInBackground);
    collector.Extend(phrase, limit, outSamples);
}

Collector *SuffixArray::NewCollector(const context_t *context, bool searchInBackground) {
    return new Collector(storage, db, prefixLength, context, searchInBackground);
}

IndexIterator *SuffixArray::NewIterator() const {
    return new IndexIterator(db, prefixLength);
}

IndexIterator::IndexIterator(rocksdb::DB *db, uint8_t prefixLength) : prefixLength(prefixLength) {
    it = db->NewIterator(rocksdb::ReadOptions());
    it->SeekToFirst();
}

IndexIterator::~IndexIterator() {
    delete it;
}

bool IndexIterator::Next(IndexIterator::IndexEntry *outEntry) {
    while (it->Valid()) {
        Slice key = it->key();
        Slice value = it->value();

        KeyType type = GetKeyTypeFromKey(key.data(), prefixLength);

        bool loop = false;

        switch (type) {
            case kSourcePrefixKeyType:
                outEntry->is_source = true;
                outEntry->domain = GetDomainFromKey(key.data(), prefixLength);

                outEntry->words.clear();
                GetWordsFromKey(key.data(), prefixLength, outEntry->words);

                outEntry->positions.clear();
                PostingList::Deserialize(value.data(), value.size(), outEntry->positions);

                outEntry->count = (int64_t) outEntry->positions.size();
                break;
            case kTargetCountKeyType:
                outEntry->is_source = false;
                outEntry->domain = 0;
                outEntry->positions.clear();

                outEntry->words.clear();
                GetWordsFromKey(key.data(), prefixLength, outEntry->words);

                outEntry->count = DeserializeCount(value.data(), value.size());
                break;
            default:
                loop = true;
                break;
        }

        it->Next();
        Status status = it->status();
        if (!status.ok())
            throw index_exception(status.ToString());

        if (!loop)
            return true;
    }

    return false;
}