# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""tvm.contrib.msc.framework.torch.frontend.translate"""

from typing import Dict, Optional, Tuple, List

import tvm
from tvm import relax
from tvm.contrib.msc.core import transform as msc_transform
from tvm.contrib.msc.core.ir import MSCGraph, byoc_partition
from tvm.contrib.msc.framework.tensorrt import transform as trt_transform


def partition_for_tensorrt(
    mod: tvm.IRModule,
    params: Optional[Dict[str, tvm.nd.array]] = None,
    trans_config: Optional[Dict[str, str]] = None,
    build_config: Optional[Dict[str, str]] = None,
    allow_incomplete: bool = True,
) -> Tuple[tvm.IRModule, List[Tuple[str, MSCGraph, Dict[str, tvm.nd.array]]]]:
    """Partition module to tensorrt sub functions.

    Parameters
    ----------
    mod: IRModule
        The IRModule of relax.
    trans_config: dict
        The config for transform IRModule.
    params: dict of <string:tvm.ndarray>
        The parameters of the IRModule.
    build_config: dict
        The config for build MSCGraph.
    allow_incomplete: bool
        Whether allow some ops not on tensorrt

    Returns
    -------
    mod: IRModule
        The IRModule of partitioned relax.
    graphs_info: list<<str, MSCGraph, weights>>
        The func <name, MSCGraph and weights> list, each element for a sub graph.
    """

    trans_config = trans_config or {}
    mod = tvm.transform.Sequential(
        [
            msc_transform.SetExprName(),
            trt_transform.TransformTensorRT(trans_config.get("version")),
            relax.transform.FoldConstant(),
        ]
    )(mod)
    return byoc_partition("msc_tensorrt", mod, params, trans_config, build_config, allow_incomplete)
