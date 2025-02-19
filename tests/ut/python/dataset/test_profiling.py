# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""
Testing profiling support in DE
"""
import json
import os
import numpy as np
import mindspore.dataset as ds

FILES = ["../data/dataset/testTFTestAllTypes/test.data"]
DATASET_ROOT = "../data/dataset/testTFTestAllTypes/"
SCHEMA_FILE = "../data/dataset/testTFTestAllTypes/datasetSchema.json"

PIPELINE_FILE = "./pipeline_profiling_1.json"
DATASET_ITERATOR_FILE = "./dataset_iterator_profiling_1.txt"


def test_profiling_simple_pipeline():
    """
    Generator -> Shuffle -> Batch
    """
    os.environ['PROFILING_MODE'] = 'true'
    os.environ['MINDDATA_PROFILING_DIR'] = '.'
    os.environ['DEVICE_ID'] = '1'

    source = [(np.array([x]),) for x in range(1024)]
    data1 = ds.GeneratorDataset(source, ["data"])
    data1 = data1.shuffle(64)
    data1 = data1.batch(32)
    # try output shape type and dataset size and make sure no profiling file is generated
    assert data1.output_shapes() == [[32, 1]]
    assert [str(tp) for tp in data1.output_types()] == ["int64"]
    assert data1.get_dataset_size() == 32
    assert os.path.exists(PIPELINE_FILE) is False
    assert os.path.exists(DATASET_ITERATOR_FILE) is False

    for _ in data1:
        pass

    assert os.path.exists(PIPELINE_FILE) is True
    os.remove(PIPELINE_FILE)
    assert os.path.exists(DATASET_ITERATOR_FILE) is True
    os.remove(DATASET_ITERATOR_FILE)
    del os.environ['PROFILING_MODE']
    del os.environ['MINDDATA_PROFILING_DIR']


def test_profiling_complex_pipeline():
    """
    Generator -> Map     ->
                             -> Zip
    TFReader  -> Shuffle ->
    """
    os.environ['PROFILING_MODE'] = 'true'
    os.environ['MINDDATA_PROFILING_DIR'] = '.'
    os.environ['DEVICE_ID'] = '1'

    source = [(np.array([x]),) for x in range(1024)]
    data1 = ds.GeneratorDataset(source, ["gen"])
    data1 = data1.map(operations=[(lambda x: x + 1)], input_columns=["gen"])

    pattern = DATASET_ROOT + "/test.data"
    data2 = ds.TFRecordDataset(pattern, SCHEMA_FILE, shuffle=ds.Shuffle.FILES)
    data2 = data2.shuffle(4)

    data3 = ds.zip((data1, data2))

    for _ in data3:
        pass

    with open(PIPELINE_FILE) as f:
        data = json.load(f)
        op_info = data["op_info"]
        assert len(op_info) == 5
        for i in range(5):
            if op_info[i]["op_type"] != "ZipOp":
                assert "size" in op_info[i]["metrics"]["output_queue"]
                assert "length" in op_info[i]["metrics"]["output_queue"]
                assert "throughput" in op_info[i]["metrics"]["output_queue"]
            else:
                # Note: Zip is an inline op and hence does not have metrics information
                assert op_info[i]["metrics"] is None

    assert os.path.exists(PIPELINE_FILE) is True
    os.remove(PIPELINE_FILE)
    assert os.path.exists(DATASET_ITERATOR_FILE) is True
    os.remove(DATASET_ITERATOR_FILE)
    del os.environ['PROFILING_MODE']
    del os.environ['MINDDATA_PROFILING_DIR']


def test_profiling_inline_ops_pipeline1():
    """
    Test pipeline with inline ops: Concat and EpochCtrl
    Generator ->
                 Concat -> EpochCtrl
    Generator ->
    """
    os.environ['PROFILING_MODE'] = 'true'
    os.environ['MINDDATA_PROFILING_DIR'] = '.'
    os.environ['DEVICE_ID'] = '1'

    # In source1 dataset: Number of rows is 3; its values are 0, 1, 2
    def source1():
        for i in range(3):
            yield (np.array([i]),)

    # In source2 dataset: Number of rows is 7; its values are 3, 4, 5 ... 9
    def source2():
        for i in range(3, 10):
            yield (np.array([i]),)

    data1 = ds.GeneratorDataset(source1, ["col1"])
    data2 = ds.GeneratorDataset(source2, ["col1"])
    data3 = data1.concat(data2)

    # Here i refers to index, d refers to data element
    for i, d in enumerate(data3.create_tuple_iterator(output_numpy=True)):
        t = d
        assert i == t[0][0]

    assert sum([1 for _ in data3]) == 10

    with open(PIPELINE_FILE) as f:
        data = json.load(f)
        op_info = data["op_info"]
        assert len(op_info) == 4
        for i in range(4):
            # Note: The following ops are inline ops: Concat, EpochCtrl
            if op_info[i]["op_type"] in ("ConcatOp", "EpochCtrlOp"):
                # Confirm these inline ops do not have metrics information
                assert op_info[i]["metrics"] is None
            else:
                assert "size" in op_info[i]["metrics"]["output_queue"]
                assert "length" in op_info[i]["metrics"]["output_queue"]
                assert "throughput" in op_info[i]["metrics"]["output_queue"]

    assert os.path.exists(PIPELINE_FILE) is True
    os.remove(PIPELINE_FILE)
    assert os.path.exists(DATASET_ITERATOR_FILE) is True
    os.remove(DATASET_ITERATOR_FILE)
    del os.environ['PROFILING_MODE']
    del os.environ['MINDDATA_PROFILING_DIR']


def test_profiling_inline_ops_pipeline2():
    """
    Test pipeline with many inline ops
    Generator -> Rename -> Skip -> Repeat -> Take
    """
    os.environ['PROFILING_MODE'] = 'true'
    os.environ['MINDDATA_PROFILING_DIR'] = '.'
    os.environ['DEVICE_ID'] = '1'

    # In source1 dataset: Number of rows is 10; its values are 0, 1, 2, 3, 4, 5 ... 9
    def source1():
        for i in range(10):
            yield (np.array([i]),)

    data1 = ds.GeneratorDataset(source1, ["col1"])
    data1 = data1.rename(input_columns=["col1"], output_columns=["newcol1"])
    data1 = data1.skip(2)
    data1 = data1.repeat(2)
    data1 = data1.take(12)

    for _ in data1:
        pass

    with open(PIPELINE_FILE) as f:
        data = json.load(f)
        op_info = data["op_info"]
        assert len(op_info) == 5
        for i in range(5):
            # Check for these inline ops
            if op_info[i]["op_type"] in ("RenameOp", "RepeatOp", "SkipOp", "TakeOp"):
                # Confirm these inline ops do not have metrics information
                assert op_info[i]["metrics"] is None
            else:
                assert "size" in op_info[i]["metrics"]["output_queue"]
                assert "length" in op_info[i]["metrics"]["output_queue"]
                assert "throughput" in op_info[i]["metrics"]["output_queue"]

    assert os.path.exists(PIPELINE_FILE) is True
    os.remove(PIPELINE_FILE)
    assert os.path.exists(DATASET_ITERATOR_FILE) is True
    os.remove(DATASET_ITERATOR_FILE)
    del os.environ['PROFILING_MODE']
    del os.environ['MINDDATA_PROFILING_DIR']


def test_profiling_sampling_interval():
    """
    Test non-default monitor sampling interval
    """
    os.environ['PROFILING_MODE'] = 'true'
    os.environ['MINDDATA_PROFILING_DIR'] = '.'
    os.environ['DEVICE_ID'] = '1'
    interval_origin = ds.config.get_monitor_sampling_interval()

    ds.config.set_monitor_sampling_interval(30)
    interval = ds.config.get_monitor_sampling_interval()
    assert interval == 30

    source = [(np.array([x]),) for x in range(1024)]
    data1 = ds.GeneratorDataset(source, ["data"])
    data1 = data1.shuffle(64)
    data1 = data1.batch(32)

    for _ in data1:
        pass

    assert os.path.exists(PIPELINE_FILE) is True
    os.remove(PIPELINE_FILE)
    assert os.path.exists(DATASET_ITERATOR_FILE) is True
    os.remove(DATASET_ITERATOR_FILE)

    ds.config.set_monitor_sampling_interval(interval_origin)
    del os.environ['PROFILING_MODE']
    del os.environ['MINDDATA_PROFILING_DIR']


if __name__ == "__main__":
    test_profiling_simple_pipeline()
    test_profiling_complex_pipeline()
    test_profiling_inline_ops_pipeline1()
    test_profiling_inline_ops_pipeline2()
    test_profiling_sampling_interval()
