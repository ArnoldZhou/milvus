// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <random>
#include <thread>
#include <fiu-control.h>
#include <fiu-local.h>

#include "cache/CpuCacheMgr.h"
#include "db/Constants.h"
#include "db/DB.h"
#include "db/DBFactory.h"
#include "db/DBImpl.h"
#include "db/IDGenerator.h"
#include "db/meta/MetaConsts.h"
#include "db/utils.h"
#include "server/Config.h"
#include "utils/CommonUtil.h"


namespace {

static const char *TABLE_NAME = "test_group";
static constexpr int64_t TABLE_DIM = 256;
static constexpr int64_t VECTOR_COUNT = 25000;
static constexpr int64_t INSERT_LOOP = 1000;
static constexpr int64_t SECONDS_EACH_HOUR = 3600;
static constexpr int64_t DAY_SECONDS = 24 * 60 * 60;

milvus::engine::meta::TableSchema
BuildTableSchema() {
    milvus::engine::meta::TableSchema table_info;
    table_info.dimension_ = TABLE_DIM;
    table_info.table_id_ = TABLE_NAME;
    return table_info;
}

void
BuildVectors(uint64_t n, uint64_t batch_index, milvus::engine::VectorsData &vectors) {
    vectors.vector_count_ = n;
    vectors.float_data_.clear();
    vectors.float_data_.resize(n * TABLE_DIM);
    float *data = vectors.float_data_.data();
    for (uint64_t i = 0; i < n; i++) {
        for (int64_t j = 0; j < TABLE_DIM; j++) data[TABLE_DIM * i + j] = drand48();
        data[TABLE_DIM * i] += i / 2000.;

        vectors.id_array_.push_back(n * batch_index + i);
    }

    //    milvus::engine::SimpleIDGenerator id_gen;
    //    id_gen.GetNextIDNumbers(n, vectors.id_array_);
}

std::string
CurrentTmDate(int64_t offset_day = 0) {
    time_t tt;
    time(&tt);
    tt = tt + 8 * SECONDS_EACH_HOUR;
    tt = tt + 24 * SECONDS_EACH_HOUR * offset_day;
    tm t;
    gmtime_r(&tt, &t);

    std::string str =
        std::to_string(t.tm_year + 1900) + "-" + std::to_string(t.tm_mon + 1) + "-" + std::to_string(t.tm_mday);

    return str;
}

void
ConvertTimeRangeToDBDates(const std::string &start_value, const std::string &end_value,
                          std::vector<milvus::engine::meta::DateT> &dates) {
    dates.clear();

    time_t tt_start, tt_end;
    tm tm_start, tm_end;
    if (!milvus::server::CommonUtil::TimeStrToTime(start_value, tt_start, tm_start)) {
        return;
    }

    if (!milvus::server::CommonUtil::TimeStrToTime(end_value, tt_end, tm_end)) {
        return;
    }

    int64_t days = (tt_end > tt_start) ? (tt_end - tt_start) / DAY_SECONDS : (tt_start - tt_end) / DAY_SECONDS;
    if (days == 0) {
        return;
    }

    for (int64_t i = 0; i < days; i++) {
        time_t tt_day = tt_start + DAY_SECONDS * i;
        tm tm_day;
        milvus::server::CommonUtil::ConvertTime(tt_day, tm_day);

        int64_t date = tm_day.tm_year * 10000 + tm_day.tm_mon * 100 + tm_day.tm_mday;  // according to db logic
        dates.push_back(date);
    }
}

}  // namespace

TEST_F(DBTest, CONFIG_TEST) {
    {
        ASSERT_ANY_THROW(milvus::engine::ArchiveConf conf("wrong"));
        /* EXPECT_DEATH(engine::ArchiveConf conf("wrong"), ""); */
    }
    {
        milvus::engine::ArchiveConf conf("delete");
        ASSERT_EQ(conf.GetType(), "delete");
        auto criterias = conf.GetCriterias();
        ASSERT_EQ(criterias.size(), 0);
    }
    {
        milvus::engine::ArchiveConf conf("swap");
        ASSERT_EQ(conf.GetType(), "swap");
        auto criterias = conf.GetCriterias();
        ASSERT_EQ(criterias.size(), 0);
    }
    {
        ASSERT_ANY_THROW(milvus::engine::ArchiveConf conf1("swap", "disk:"));
        ASSERT_ANY_THROW(milvus::engine::ArchiveConf conf2("swap", "disk:a"));
        milvus::engine::ArchiveConf conf("swap", "disk:1024");
        auto criterias = conf.GetCriterias();
        ASSERT_EQ(criterias.size(), 1);
        ASSERT_EQ(criterias["disk"], 1024);
    }
    {
        ASSERT_ANY_THROW(milvus::engine::ArchiveConf conf1("swap", "days:"));
        ASSERT_ANY_THROW(milvus::engine::ArchiveConf conf2("swap", "days:a"));
        milvus::engine::ArchiveConf conf("swap", "days:100");
        auto criterias = conf.GetCriterias();
        ASSERT_EQ(criterias.size(), 1);
        ASSERT_EQ(criterias["days"], 100);
    }
    {
        ASSERT_ANY_THROW(milvus::engine::ArchiveConf conf1("swap", "days:"));
        ASSERT_ANY_THROW(milvus::engine::ArchiveConf conf2("swap", "days:a"));
        milvus::engine::ArchiveConf conf("swap", "days:100;disk:200");
        auto criterias = conf.GetCriterias();
        ASSERT_EQ(criterias.size(), 2);
        ASSERT_EQ(criterias["days"], 100);
        ASSERT_EQ(criterias["disk"], 200);
    }
}

TEST_F(DBTest, DB_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);

    milvus::engine::meta::TableSchema table_info_get;
    table_info_get.table_id_ = TABLE_NAME;
    stat = db_->DescribeTable(table_info_get);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(table_info_get.dimension_, TABLE_DIM);

    uint64_t qb = 5;
    milvus::engine::VectorsData qxb;
    BuildVectors(qb, 0, qxb);

    std::thread search([&]() {
        milvus::engine::ResultIds result_ids;
        milvus::engine::ResultDistances result_distances;
        int k = 10;
        std::this_thread::sleep_for(std::chrono::seconds(2));

        INIT_TIMER;
        std::stringstream ss;
        uint64_t count = 0;
        uint64_t prev_count = 0;

        for (auto j = 0; j < 10; ++j) {
            ss.str("");
            db_->Size(count);
            prev_count = count;

            START_TIMER;

            std::vector<std::string> tags;
            stat = db_->Query(dummy_context_, TABLE_NAME, tags, k, 10, qxb, result_ids, result_distances);
            ss << "Search " << j << " With Size " << count / milvus::engine::M << " M";
            STOP_TIMER(ss.str());

            ASSERT_TRUE(stat.ok());
            ASSERT_EQ(result_ids.size(), qb * k);
            for (auto i = 0; i < qb; ++i) {
                ss.str("");
                ss << "Result [" << i << "]:";
                for (auto t = 0; t < k; t++) {
                    ss << result_ids[i * k + t] << " ";
                }
                /* LOG(DEBUG) << ss.str(); */
            }
            ASSERT_TRUE(count >= prev_count);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    int loop = INSERT_LOOP;

    for (auto i = 0; i < loop; ++i) {
        if (i == 40) {
            db_->InsertVectors(TABLE_NAME, "", qxb);
            ASSERT_EQ(qxb.id_array_.size(), qb);
        } else {
            uint64_t nb = 50;
            milvus::engine::VectorsData xb;
            BuildVectors(nb, i, xb);

            db_->InsertVectors(TABLE_NAME, "", xb);
            ASSERT_EQ(xb.id_array_.size(), nb);
        }

        stat = db_->Flush();
        ASSERT_TRUE(stat.ok());

        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }

    search.join();

    uint64_t count;
    stat = db_->GetTableRowCount(TABLE_NAME, count);
    ASSERT_TRUE(stat.ok());
    ASSERT_GT(count, 0);
}

TEST_F(DBTest, SEARCH_TEST) {
    milvus::scheduler::OptimizerInst::GetInstance()->Init();
    std::string config_path(CONFIG_PATH);
    config_path += CONFIG_FILE;
    milvus::server::Config &config = milvus::server::Config::GetInstance();
    milvus::Status s = config.LoadConfigFile(config_path);

    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);

    milvus::engine::meta::TableSchema table_info_get;
    table_info_get.table_id_ = TABLE_NAME;
    stat = db_->DescribeTable(table_info_get);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(table_info_get.dimension_, TABLE_DIM);

    // prepare raw data
    size_t nb = VECTOR_COUNT;
    size_t nq = 10;
    size_t k = 5;
    milvus::engine::VectorsData xb, xq;
    xb.vector_count_ = nb;
    xb.float_data_.resize(nb * TABLE_DIM);
    xq.vector_count_ = nq;
    xq.float_data_.resize(nq * TABLE_DIM);
    xb.id_array_.resize(nb);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis_xt(-1.0, 1.0);
    for (size_t i = 0; i < nb * TABLE_DIM; i++) {
        xb.float_data_[i] = dis_xt(gen);
        if (i < nb) {
            xb.id_array_[i] = i;
        }
    }
    for (size_t i = 0; i < nq * TABLE_DIM; i++) {
        xq.float_data_[i] = dis_xt(gen);
    }

    // result data
    // std::vector<long> nns_gt(k*nq);
    std::vector<int64_t> nns(k * nq);  // nns = nearst neg search
    // std::vector<float> dis_gt(k*nq);
    std::vector<float> dis(k * nq);

    // insert data
    stat = db_->InsertVectors(TABLE_NAME, "", xb);
    ASSERT_TRUE(stat.ok());

    milvus::engine::TableIndex index;
    index.engine_type_ = (int) milvus::engine::EngineType::FAISS_IDMAP;
    db_->CreateIndex(TABLE_NAME, index);  // wait until build index finish

    {
        std::vector<std::string> tags;
        milvus::engine::ResultIds result_ids;
        milvus::engine::ResultDistances result_distances;
        stat = db_->Query(dummy_context_, TABLE_NAME, tags, k, 10, xq, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
    }

    index.engine_type_ = (int) milvus::engine::EngineType::FAISS_IVFFLAT;
    db_->CreateIndex(TABLE_NAME, index);  // wait until build index finish

    {
        std::vector<std::string> tags;
        milvus::engine::ResultIds result_ids;
        milvus::engine::ResultDistances result_distances;
        stat = db_->Query(dummy_context_, TABLE_NAME, tags, k, 10, xq, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
    }

    index.engine_type_ = (int) milvus::engine::EngineType::FAISS_IVFSQ8;
    db_->CreateIndex(TABLE_NAME, index);  // wait until build index finish

    {
        std::vector<std::string> tags;
        milvus::engine::ResultIds result_ids;
        milvus::engine::ResultDistances result_distances;
        stat = db_->Query(dummy_context_, TABLE_NAME, tags, k, 10, xq, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
    }

#ifdef CUSTOMIZATION
#ifdef MILVUS_GPU_VERSION
    index.engine_type_ = (int)milvus::engine::EngineType::FAISS_IVFSQ8H;
    db_->CreateIndex(TABLE_NAME, index);  // wait until build index finish

    {
        std::vector<std::string> tags;
        milvus::engine::ResultIds result_ids;
        milvus::engine::ResultDistances result_distances;
        stat = db_->Query(dummy_context_, TABLE_NAME, tags, k, 10, xq, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
    }
#endif
#endif

    {  // search by specify index file
        milvus::engine::meta::DatesT dates;
        std::vector<std::string> file_ids;
        // sometimes this case run fast to merge file and build index, old file will be deleted immediately,
        // so the QueryByFileID cannot get files to search
        // input 100 files ids to avoid random failure of this case
        for (int i = 0; i < 100; i++) {
            file_ids.push_back(std::to_string(i));
        }
        milvus::engine::ResultIds result_ids;
        milvus::engine::ResultDistances result_distances;
        stat = db_->QueryByFileID(dummy_context_, TABLE_NAME, file_ids, k, 10, xq, dates, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
    }

    // TODO(zhiru): PQ build takes forever

    //    index.engine_type_ = (int)milvus::engine::EngineType::FAISS_PQ;
    //    db_->CreateIndex(TABLE_NAME, index);  // wait until build index finish
    //
    //    {
    //        std::vector<std::string> tags;
    //        milvus::engine::ResultIds result_ids;
    //        milvus::engine::ResultDistances result_distances;
    //        stat = db_->Query(dummy_context_, TABLE_NAME, tags, k, 10, xq, result_ids, result_distances);
    //        ASSERT_TRUE(stat.ok());
    //    }

#ifdef CUSTOMIZATION
#ifdef MILVUS_GPU_VERSION
    // test FAISS_IVFSQ8H optimizer
    index.engine_type_ = (int)milvus::engine::EngineType::FAISS_IVFSQ8H;
    db_->CreateIndex(TABLE_NAME, index);  // wait until build index finish
    std::vector<std::string> partition_tag;
    milvus::engine::ResultIds result_ids;
    milvus::engine::ResultDistances result_dists;

    {
        result_ids.clear();
        result_dists.clear();
        stat = db_->Query(dummy_context_, TABLE_NAME, partition_tag, k, 10, xq, result_ids, result_dists);
        ASSERT_TRUE(stat.ok());
    }

    {  // search by specify index file
        milvus::engine::meta::DatesT dates;
        std::vector<std::string> file_ids;
        // sometimes this case run fast to merge file and build index, old file will be deleted immediately,
        // so the QueryByFileID cannot get files to search
        // input 100 files ids to avoid random failure of this case
        for (int i = 0; i < 100; i++) {
            file_ids.push_back(std::to_string(i));
        }
        result_ids.clear();
        result_dists.clear();
        stat = db_->QueryByFileID(dummy_context_, TABLE_NAME, file_ids, k, 10, xq, dates, result_ids, result_dists);
        ASSERT_TRUE(stat.ok());
    }
#endif
#endif
}

TEST_F(DBTest, PRELOADTABLE_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);

    milvus::engine::meta::TableSchema table_info_get;
    table_info_get.table_id_ = TABLE_NAME;
    stat = db_->DescribeTable(table_info_get);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(table_info_get.dimension_, TABLE_DIM);

    int loop = 5;
    for (auto i = 0; i < loop; ++i) {
        uint64_t nb = VECTOR_COUNT;
        milvus::engine::VectorsData xb;
        BuildVectors(nb, i, xb);

        db_->InsertVectors(TABLE_NAME, "", xb);
        ASSERT_EQ(xb.id_array_.size(), nb);
    }

    milvus::engine::TableIndex index;
    index.engine_type_ = (int) milvus::engine::EngineType::FAISS_IDMAP;
    db_->CreateIndex(TABLE_NAME, index);  // wait until build index finish

    int64_t prev_cache_usage = milvus::cache::CpuCacheMgr::GetInstance()->CacheUsage();
    stat = db_->PreloadTable(TABLE_NAME);
    ASSERT_TRUE(stat.ok());
    int64_t cur_cache_usage = milvus::cache::CpuCacheMgr::GetInstance()->CacheUsage();
    ASSERT_TRUE(prev_cache_usage < cur_cache_usage);
}

TEST_F(DBTest, SHUTDOWN_TEST) {
    db_->Stop();

    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);
    ASSERT_FALSE(stat.ok());

    stat = db_->DescribeTable(table_info);
    ASSERT_FALSE(stat.ok());

    bool has_table = false;
    stat = db_->HasTable(table_info.table_id_, has_table);
    ASSERT_FALSE(stat.ok());

    milvus::engine::VectorsData xb;
    stat = db_->InsertVectors(table_info.table_id_, "", xb);
    ASSERT_FALSE(stat.ok());

    stat = db_->Flush();
    ASSERT_FALSE(stat.ok());

    stat = db_->DeleteVector(table_info.table_id_, 0);
    ASSERT_FALSE(stat.ok());

    milvus::engine::IDNumbers ids_to_delete{0};
    stat = db_->DeleteVectors(table_info.table_id_, ids_to_delete);
    ASSERT_FALSE(stat.ok());

    stat = db_->Compact(table_info.table_id_);
    ASSERT_FALSE(stat.ok());

    stat = db_->PreloadTable(table_info.table_id_);
    ASSERT_FALSE(stat.ok());

    uint64_t row_count = 0;
    stat = db_->GetTableRowCount(table_info.table_id_, row_count);
    ASSERT_FALSE(stat.ok());

    milvus::engine::TableIndex index;
    stat = db_->CreateIndex(table_info.table_id_, index);
    ASSERT_FALSE(stat.ok());

    stat = db_->DescribeIndex(table_info.table_id_, index);
    ASSERT_FALSE(stat.ok());

    std::vector<std::string> tags;
    milvus::engine::meta::DatesT dates;
    milvus::engine::ResultIds result_ids;
    milvus::engine::ResultDistances result_distances;
    stat = db_->Query(dummy_context_, table_info.table_id_, tags, 1, 1, xb, dates, result_ids, result_distances);
    ASSERT_FALSE(stat.ok());
    std::vector<std::string> file_ids;
    stat = db_->QueryByFileID(dummy_context_, table_info.table_id_, file_ids, 1, 1, xb, dates, result_ids,
                              result_distances);
    ASSERT_FALSE(stat.ok());

    stat = db_->DropTable(table_info.table_id_, dates);
    ASSERT_FALSE(stat.ok());
}

TEST_F(DBTest, INDEX_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);

    uint64_t nb = VECTOR_COUNT;
    milvus::engine::VectorsData xb;
    BuildVectors(nb, 0, xb);

    db_->InsertVectors(TABLE_NAME, "", xb);
    ASSERT_EQ(xb.id_array_.size(), nb);

    milvus::engine::TableIndex index;
    index.engine_type_ = (int) milvus::engine::EngineType::FAISS_IVFSQ8;
    index.metric_type_ = (int) milvus::engine::MetricType::IP;
    stat = db_->CreateIndex(table_info.table_id_, index);
    ASSERT_TRUE(stat.ok());

    index.engine_type_ = (int) milvus::engine::EngineType::FAISS_IVFFLAT;
    stat = db_->CreateIndex(table_info.table_id_, index);
    ASSERT_TRUE(stat.ok());

#ifdef CUSTOMIZATION
#ifdef MILVUS_GPU_VERSION
    index.engine_type_ = (int)milvus::engine::EngineType::FAISS_IVFSQ8H;
    stat = db_->CreateIndex(table_info.table_id_, index);
    ASSERT_TRUE(stat.ok());
#endif
#endif

    milvus::engine::TableIndex index_out;
    stat = db_->DescribeIndex(table_info.table_id_, index_out);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(index.engine_type_, index_out.engine_type_);
    ASSERT_EQ(index.nlist_, index_out.nlist_);
    ASSERT_EQ(table_info.metric_type_, index_out.metric_type_);

    stat = db_->DropIndex(table_info.table_id_);
    ASSERT_TRUE(stat.ok());
}

TEST_F(DBTest, PARTITION_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);
    ASSERT_TRUE(stat.ok());

    // create partition and insert data
    const int64_t PARTITION_COUNT = 5;
    const int64_t INSERT_BATCH = 2000;
    std::string table_name = TABLE_NAME;
    for (int64_t i = 0; i < PARTITION_COUNT; i++) {
        std::string partition_tag = std::to_string(i);
        std::string partition_name = table_name + "_" + partition_tag;
        stat = db_->CreatePartition(table_name, partition_name, partition_tag);
        ASSERT_TRUE(stat.ok());

        // not allow nested partition
        stat = db_->CreatePartition(partition_name, "dumy", "dummy");
        ASSERT_FALSE(stat.ok());

        // not allow duplicated partition
        stat = db_->CreatePartition(table_name, partition_name, partition_tag);
        ASSERT_FALSE(stat.ok());

        milvus::engine::VectorsData xb;
        BuildVectors(INSERT_BATCH, i, xb);

        milvus::engine::IDNumbers vector_ids;
        vector_ids.resize(INSERT_BATCH);
        for (int64_t k = 0; k < INSERT_BATCH; k++) {
            vector_ids[k] = i * INSERT_BATCH + k;
        }

        db_->InsertVectors(table_name, partition_tag, xb);
        ASSERT_EQ(vector_ids.size(), INSERT_BATCH);
    }

    // duplicated partition is not allowed
    stat = db_->CreatePartition(table_name, "", "0");
    ASSERT_FALSE(stat.ok());

    std::vector<milvus::engine::meta::TableSchema> partition_schema_array;
    stat = db_->ShowPartitions(table_name, partition_schema_array);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(partition_schema_array.size(), PARTITION_COUNT);
    for (int64_t i = 0; i < PARTITION_COUNT; i++) {
        ASSERT_EQ(partition_schema_array[i].table_id_, table_name + "_" + std::to_string(i));
    }

    {  // build index
        milvus::engine::TableIndex index;
        index.engine_type_ = (int) milvus::engine::EngineType::FAISS_IVFFLAT;
        index.metric_type_ = (int) milvus::engine::MetricType::L2;
        stat = db_->CreateIndex(table_info.table_id_, index);
        ASSERT_TRUE(stat.ok());

        uint64_t row_count = 0;
        stat = db_->GetTableRowCount(TABLE_NAME, row_count);
        ASSERT_TRUE(stat.ok());
        ASSERT_EQ(row_count, INSERT_BATCH * PARTITION_COUNT);
    }

    {  // search
        const int64_t nq = 5;
        const int64_t topk = 10;
        const int64_t nprobe = 10;
        milvus::engine::VectorsData xq;
        BuildVectors(nq, 0, xq);

        // specify partition tags
        std::vector<std::string> tags = {"0", std::to_string(PARTITION_COUNT - 1)};
        milvus::engine::ResultIds result_ids;
        milvus::engine::ResultDistances result_distances;
        stat = db_->Query(dummy_context_, TABLE_NAME, tags, topk, nprobe, xq, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
        ASSERT_EQ(result_ids.size() / topk, nq);

        // search in whole table
        tags.clear();
        result_ids.clear();
        result_distances.clear();
        stat = db_->Query(dummy_context_, TABLE_NAME, tags, topk, nprobe, xq, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
        ASSERT_EQ(result_ids.size() / topk, nq);

        // search in all partitions(tag regex match)
        tags.push_back("\\d");
        result_ids.clear();
        result_distances.clear();
        stat = db_->Query(dummy_context_, TABLE_NAME, tags, topk, nprobe, xq, result_ids, result_distances);
        ASSERT_TRUE(stat.ok());
        ASSERT_EQ(result_ids.size() / topk, nq);
    }

    stat = db_->DropPartition(table_name + "_0");
    ASSERT_TRUE(stat.ok());

    stat = db_->DropPartitionByTag(table_name, "1");
    ASSERT_TRUE(stat.ok());

    stat = db_->DropIndex(table_name);
    ASSERT_TRUE(stat.ok());

    milvus::engine::meta::DatesT dates;
    stat = db_->DropTable(table_name, dates);
    ASSERT_TRUE(stat.ok());
}

TEST_F(DBTest2, ARHIVE_DISK_CHECK) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);

    std::vector<milvus::engine::meta::TableSchema> table_schema_array;
    stat = db_->AllTables(table_schema_array);
    ASSERT_TRUE(stat.ok());
    bool bfound = false;
    for (auto &schema : table_schema_array) {
        if (schema.table_id_ == TABLE_NAME) {
            bfound = true;
            break;
        }
    }
    ASSERT_TRUE(bfound);

    milvus::engine::meta::TableSchema table_info_get;
    table_info_get.table_id_ = TABLE_NAME;
    stat = db_->DescribeTable(table_info_get);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(table_info_get.dimension_, TABLE_DIM);

    uint64_t size;
    db_->Size(size);

    int loop = INSERT_LOOP;
    for (auto i = 0; i < loop; ++i) {
        uint64_t nb = 10;
        milvus::engine::VectorsData xb;
        BuildVectors(nb, i, xb);

        db_->InsertVectors(TABLE_NAME, "", xb);
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    db_->Size(size);
    LOG(DEBUG) << "size=" << size;
    ASSERT_LE(size, 1 * milvus::engine::G);
}

TEST_F(DBTest2, DELETE_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);

    milvus::engine::meta::TableSchema table_info_get;
    table_info_get.table_id_ = TABLE_NAME;
    stat = db_->DescribeTable(table_info_get);
    ASSERT_TRUE(stat.ok());

    bool has_table = false;
    db_->HasTable(TABLE_NAME, has_table);
    ASSERT_TRUE(has_table);

    uint64_t size;
    db_->Size(size);

    uint64_t nb = VECTOR_COUNT;
    milvus::engine::VectorsData xb;
    BuildVectors(nb, 0, xb);

    milvus::engine::IDNumbers vector_ids;
    stat = db_->InsertVectors(TABLE_NAME, "", xb);
    milvus::engine::TableIndex index;
    stat = db_->CreateIndex(TABLE_NAME, index);

    std::vector<milvus::engine::meta::DateT> dates;
    stat = db_->DropTable(TABLE_NAME, dates);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ASSERT_TRUE(stat.ok());

    db_->HasTable(TABLE_NAME, has_table);
    ASSERT_FALSE(has_table);
}

TEST_F(DBTest2, DELETE_BY_RANGE_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);

    milvus::engine::meta::TableSchema table_info_get;
    table_info_get.table_id_ = TABLE_NAME;
    stat = db_->DescribeTable(table_info_get);
    ASSERT_TRUE(stat.ok());

    bool has_table = false;
    db_->HasTable(TABLE_NAME, has_table);
    ASSERT_TRUE(has_table);

    uint64_t size;
    db_->Size(size);
    ASSERT_EQ(size, 0UL);

    uint64_t nb = VECTOR_COUNT;
    milvus::engine::VectorsData xb;
    BuildVectors(nb, 0, xb);

    milvus::engine::IDNumbers vector_ids;
    stat = db_->InsertVectors(TABLE_NAME, "", xb);
    milvus::engine::TableIndex index;
    stat = db_->CreateIndex(TABLE_NAME, index);

    db_->Size(size);
    ASSERT_NE(size, 0UL);

    std::vector<milvus::engine::meta::DateT> dates;
    std::string start_value = CurrentTmDate(-5);
    std::string end_value = CurrentTmDate(5);
    ConvertTimeRangeToDBDates(start_value, end_value, dates);

    stat = db_->DropTable(TABLE_NAME, dates);
    ASSERT_TRUE(stat.ok());

    uint64_t row_count = 0;
    db_->GetTableRowCount(TABLE_NAME, row_count);
    ASSERT_EQ(row_count, 0UL);
}

TEST_F(DBTest2, SHOW_TABLE_INFO_TEST) {
    std::string table_name = TABLE_NAME;
    milvus::engine::meta::TableSchema table_schema = BuildTableSchema();
    auto stat = db_->CreateTable(table_schema);

    uint64_t nb = VECTOR_COUNT;
    milvus::engine::VectorsData xb;
    BuildVectors(nb, 0, xb);

    milvus::engine::IDNumbers vector_ids;
    stat = db_->InsertVectors(table_name, "", xb);

    // create partition and insert data
    const int64_t PARTITION_COUNT = 2;
    const int64_t INSERT_BATCH = 2000;
    for (int64_t i = 0; i < PARTITION_COUNT; i++) {
        std::string partition_tag = std::to_string(i);
        std::string partition_name = table_name + "_" + partition_tag;
        stat = db_->CreatePartition(table_name, partition_name, partition_tag);
        ASSERT_TRUE(stat.ok());

        milvus::engine::VectorsData xb;
        BuildVectors(INSERT_BATCH, i, xb);

        db_->InsertVectors(table_name, partition_tag, xb);
    }

    stat = db_->Flush();
    ASSERT_TRUE(stat.ok());

    {
        milvus::engine::TableInfo table_info;
        stat = db_->GetTableInfo(table_name, table_info);
        ASSERT_TRUE(stat.ok());
        ASSERT_TRUE(table_info.native_stat_.name_ == table_name);
        ASSERT_FALSE(table_info.native_stat_.segments_stat_.empty());
        int64_t row_count = 0;
        for (auto &stat : table_info.native_stat_.segments_stat_) {
            row_count += stat.row_count_;
            ASSERT_EQ(stat.index_name_, "IDMAP");
            ASSERT_GT(stat.data_size_, 0);
        }
        ASSERT_EQ(row_count, VECTOR_COUNT);

        for (auto &part : table_info.partitions_stat_) {
            row_count = 0;
            for (auto &stat : part.segments_stat_) {
                row_count += stat.row_count_;
                ASSERT_EQ(stat.index_name_, "IDMAP");
                ASSERT_GT(stat.data_size_, 0);
            }
            ASSERT_EQ(row_count, INSERT_BATCH);
        }
    }
}

TEST_F(DBTestWAL, DB_INSERT_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);
    ASSERT_TRUE(stat.ok());

    uint64_t qb = 100;
    milvus::engine::VectorsData qxb;
    BuildVectors(qb, 0, qxb);


    std::string partition_name = "part_name";
    std::string partition_tag = "part_tag";
    stat = db_->CreatePartition(table_info.table_id_, partition_name, partition_tag);
    ASSERT_TRUE(stat.ok());

    stat = db_->InsertVectors(table_info.table_id_, partition_tag, qxb);
    ASSERT_TRUE(stat.ok());

    stat = db_->InsertVectors(table_info.table_id_, "", qxb);
    ASSERT_TRUE(stat.ok());

    stat = db_->InsertVectors(table_info.table_id_, "not exist", qxb);
    ASSERT_FALSE(stat.ok());

    db_->Flush(table_info.table_id_);

    std::vector<milvus::engine::meta::DateT> dates;
    stat = db_->DropTable(table_info.table_id_, dates);
    ASSERT_TRUE(stat.ok());
}

TEST_F(DBTestWAL, DB_STOP_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);
    ASSERT_TRUE(stat.ok());

    uint64_t qb = 100;
    milvus::engine::VectorsData qxb;
    BuildVectors(qb, 0, qxb);

    for (int i = 0; i < 5; i++) {
        stat = db_->InsertVectors(table_info.table_id_, "", qxb);
        ASSERT_TRUE(stat.ok());
    }

    db_->Stop();
    db_->Start();

    const int64_t topk = 10;
    const int64_t nprobe = 10;
    milvus::engine::ResultIds result_ids;
    milvus::engine::ResultDistances result_distances;
    stat = db_->Query(dummy_context_, table_info.table_id_, {}, topk, nprobe, qxb, result_ids, result_distances);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(result_ids.size() / topk, qb);

    std::vector<milvus::engine::meta::DateT> dates;
    stat = db_->DropTable(table_info.table_id_, dates);
    ASSERT_TRUE(stat.ok());
}

TEST_F(DBTestWALRecovery, RECOVERY_WITH_NO_ERROR) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);
    ASSERT_TRUE(stat.ok());

    uint64_t qb = 100;
    milvus::engine::VectorsData qxb;
    BuildVectors(qb, 0, qxb);

    for (int i = 0; i < 5; i++) {
        stat = db_->InsertVectors(table_info.table_id_, "", qxb);
        ASSERT_TRUE(stat.ok());
    }

    const int64_t topk = 10;
    const int64_t nprobe = 10;
    milvus::engine::ResultIds result_ids;
    milvus::engine::ResultDistances result_distances;

    stat = db_->Query(dummy_context_, table_info.table_id_, {}, topk, nprobe, qxb, result_ids, result_distances);
    ASSERT_TRUE(stat.ok());
    ASSERT_NE(result_ids.size() / topk, qb);

    fiu_init(0);
    fiu_enable("DBImpl.ExexWalRecord.return", 1, nullptr, 0);
    db_ = nullptr;
    fiu_disable("DBImpl.ExexWalRecord.return");
    auto options = GetOptions();
    db_ = milvus::engine::DBFactory::Build(options);

    result_ids.clear();
    result_distances.clear();
    stat = db_->Query(dummy_context_, table_info.table_id_, {}, topk, nprobe, qxb, result_ids, result_distances);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(result_ids.size(), 0);

    db_->Flush();
    result_ids.clear();
    result_distances.clear();
    stat = db_->Query(dummy_context_, table_info.table_id_, {}, topk, nprobe, qxb, result_ids, result_distances);
    ASSERT_TRUE(stat.ok());
    ASSERT_EQ(result_ids.size() / topk, qb);
}

TEST_F(DBTestWALRecovery_Error, RECOVERY_WITH_INVALID_LOG_FILE) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);
    ASSERT_TRUE(stat.ok());

    uint64_t qb = 100;
    milvus::engine::VectorsData qxb;
    BuildVectors(qb, 0, qxb);

    stat = db_->InsertVectors(table_info.table_id_, "", qxb);
    ASSERT_TRUE(stat.ok());

    fiu_init(0);
    fiu_enable("DBImpl.ExexWalRecord.return", 1, nullptr, 0);
    db_ = nullptr;
    fiu_disable("DBImpl.ExexWalRecord.return");

    auto options = GetOptions();
    //delete wal log file so that recovery will failed when start db next time.
    boost::filesystem::remove(options.mxlog_path_ + "0.wal");
    ASSERT_ANY_THROW(db_ = milvus::engine::DBFactory::Build(options));
}

TEST_F(DBTest2, FLUSH_NON_EXISTING_TABLE) {
    auto status = db_->Flush("non_existing_table");
    ASSERT_FALSE(status.ok());
}

TEST_F(DBTest2, GET_VECTOR_BY_ID_TEST) {
    milvus::engine::meta::TableSchema table_info = BuildTableSchema();
    auto stat = db_->CreateTable(table_info);
    ASSERT_TRUE(stat.ok());

    uint64_t qb = 1000;
    milvus::engine::VectorsData qxb;
    BuildVectors(qb, 0, qxb);

    std::string partition_name = "part_name";
    std::string partition_tag = "part_tag";
    stat = db_->CreatePartition(table_info.table_id_, partition_name, partition_tag);
    ASSERT_TRUE(stat.ok());

    stat = db_->InsertVectors(table_info.table_id_, partition_tag, qxb);
    ASSERT_TRUE(stat.ok());

    db_->Flush(table_info.table_id_);

//    milvus::engine::VectorsData vector_data;
//    stat = db_->GetVectorByID(TABLE_NAME, qxb.id_array_[0], vector_data);
//    ASSERT_TRUE(stat.ok());
//    ASSERT_EQ(vector_data.vector_count_, 1);
//    ASSERT_EQ(vector_data.float_data_.size(), TABLE_DIM);
//
//    for (int64_t i = 0; i < TABLE_DIM; i++) {
//        ASSERT_EQ(vector_data.float_data_[i], qxb.float_data_[i]);
//    }
}
