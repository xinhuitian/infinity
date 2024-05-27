#include "unit_test/base_test.h"
#include <cassert>

import stl;

import posting_byte_slice;
import posting_byte_slice_reader;
import term_meta;
import file_writer;
import file_reader;
import index_defines;
import posting_list_format;
import local_file_system;

using namespace infinity;

class TermMetaTest : public BaseTest {
public:
    void SetUp() override { file_name_ = String(GetTmpDir()) + "/term_meta"; }
    void TearDown() override {}

    void DoTest1() {
        SharedPtr<FileWriter> file_writer = MakeShared<FileWriter>(fs_, file_name_, 128);
        TermMeta term_meta(1, 2, 3);
        optionflag_t option_flag = OPTION_FLAG_ALL;
        PostingFormatOption format_option(option_flag);
        TermMetaDumper term_dumper(format_option);
        term_dumper.Dump(file_writer, term_meta);
        file_writer->Sync();

        SharedPtr<FileReader> file_reader = MakeShared<FileReader>(fs_, file_name_, 128);

        TermMeta new_term_meta;
        TermMetaLoader term_loader(format_option);
        term_loader.Load(file_reader, new_term_meta);
        ASSERT_EQ(term_meta.doc_freq_, new_term_meta.doc_freq_);
        ASSERT_EQ(term_meta.total_tf_, new_term_meta.total_tf_);
        ASSERT_EQ(term_meta.payload_, new_term_meta.payload_);

        fs_.DeleteFile(file_name_);
    }

protected:
    String file_name_;
    LocalFileSystem fs_;
};

TEST_F(TermMetaTest, test1) { DoTest1(); }