# Copyright 2021 Huawei Technologies Co., Ltd
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
Watchpoints test script for offline debugger APIs.
"""

import mindspore.offline_debug.dbg_services as d
import pytest
from dump_test_utils import compare_actual_with_expected

GENERATE_GOLDEN = False
test_name = "async_sink_mode_true_watchpoints"


@pytest.mark.level0
@pytest.mark.platform_arm_ascend_training
@pytest.mark.platform_x86_ascend_training
@pytest.mark.env_onecard
def test_async_sink_mode_true_watchpoints():
    if GENERATE_GOLDEN:
        f_write = open(test_name + ".expected", "w")
    else:
        f_write = open(test_name + ".actual", "w")

    debugger_backend = d.DbgServices(
        dump_file_path="/home/workspace/mindspore_dataset/dumps/async_sink_true/")

    _ = debugger_backend.initialize(net_name="alexnet", is_sync_mode=False)

    # NOTES:
    # -> watch_condition=6 is MIN_LT
    # -> watch_condition=18 is CHANGE_TOO_LARGE

    # test 1: watchpoint set and hit (watch_condition=6)
    param1 = d.Parameter(name="param", disabled=False, value=0.0)
    _ = debugger_backend.add_watchpoint(watchpoint_id=1, watch_condition=6,
                                        check_node_list={"Default/network-TrainOneStepCell/network-WithLossCell/"
                                                         "_backbone-AlexNet/conv3-Conv2d/Conv2D-op169":
                                                             {"device_id": [0], "root_graph_id": [1],
                                                              "is_parameter": False
                                                              }}, parameter_list=[param1])

    watchpoint_hits_test_1 = debugger_backend.check_watchpoints(iteration=2)
    if len(watchpoint_hits_test_1) != 1:
        f_write.write("ERROR -> test 1: watchpoint set but not hit just once\n")
    print_watchpoint_hits(watchpoint_hits_test_1, 1, f_write)

    # test 2: watchpoint remove and ensure it's not hit
    _ = debugger_backend.remove_watchpoint(watchpoint_id=1)
    watchpoint_hits_test_2 = debugger_backend.check_watchpoints(iteration=2)
    if watchpoint_hits_test_2:
        f_write.write("ERROR -> test 2: watchpoint removed but hit\n")

    # test 3: watchpoint set and not hit, then remove
    param2 = d.Parameter(name="param", disabled=False, value=-1000.0)
    _ = debugger_backend.add_watchpoint(watchpoint_id=2, watch_condition=6,
                                        check_node_list={"Default/network-TrainOneStepCell/network-WithLossCell/"
                                                         "_backbone-AlexNet/conv3-Conv2d/Conv2D-op169":
                                                             {"device_id": [0], "root_graph_id": [1],
                                                              "is_parameter": False
                                                              }}, parameter_list=[param2])

    watchpoint_hits_test_3 = debugger_backend.check_watchpoints(iteration=2)
    if watchpoint_hits_test_3:
        f_write.write("ERROR -> test 3: watchpoint set but not supposed to be hit\n")
    _ = debugger_backend.remove_watchpoint(watchpoint_id=2)
    f_write.close()
    if not GENERATE_GOLDEN:
        assert compare_actual_with_expected(test_name)


def print_watchpoint_hits(watchpoint_hits, test_id, f_write):
    """Print watchpoint hits."""
    for x, _ in enumerate(watchpoint_hits):
        f_write.write("-----------------------------------------------------------\n")
        f_write.write("watchpoint_hit for test_%u attributes:" % test_id + "\n")
        f_write.write("name =  " + watchpoint_hits[x].name + "\n")
        f_write.write("slot =  " + str(watchpoint_hits[x].slot) + "\n")
        f_write.write("condition =  " + str(watchpoint_hits[x].condition) + "\n")
        f_write.write("watchpoint_id =  " + str(watchpoint_hits[x].watchpoint_id) + "\n")
        for p, _ in enumerate(watchpoint_hits[x].parameters):
            f_write.write("parameter  " + str(p) + "  name =  " +
                          watchpoint_hits[x].parameters[p].name + "\n")
            f_write.write("parameter  " + str(p) + "  disabled =  " +
                          str(watchpoint_hits[x].parameters[p].disabled) + "\n")
            f_write.write("parameter  " + str(p) + "  value =  " +
                          str(watchpoint_hits[x].parameters[p].value) + "\n")
            f_write.write("parameter  " + str(p) + "  hit =  " +
                          str(watchpoint_hits[x].parameters[p].hit) + "\n")
            f_write.write("parameter  " + str(p) + "  actual_value =  " +
                          str(watchpoint_hits[x].parameters[p].actual_value) + "\n")
        f_write.write("error code =  " + str(watchpoint_hits[x].error_code) + "\n")
        f_write.write("device_id =  " + str(watchpoint_hits[x].device_id) + "\n")
        f_write.write("root_graph_id =  " + str(watchpoint_hits[x].root_graph_id) + "\n")
