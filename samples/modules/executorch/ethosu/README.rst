.. zephyr:code-sample:: executorch-ethosu
   :name: ExecuTorch with Ethos-U NPU
   :relevant-api: executorch

   Run ExecuTorch inference on Cortex-A with Ethos-U85 NPU delegation
   and XNNPACK CPU backend.

Overview
********

This sample demonstrates ExecuTorch model inference on the
Corstone-1000-A320 FVP (Cortex-A320 + Ethos-U85 NPU).

Two inference paths are supported:

- **Ethos-U85 NPU delegate** — the model is compiled with the Vela
  compiler and delegated to the NPU for hardware-accelerated inference.
- **XNNPACK CPU backend** — the model runs on the Cortex-A CPU using
  NEON-optimized microkernels via KleidiAI.

Requirements
************

- Corstone-1000-A320 FVP (see :ref:`fvp_corstone1000`)
- ExecuTorch module with Zephyr Cortex-A support

Building — Ethos-U NPU (default)
*********************************

.. code-block:: console

   west build -b fvp_corstone1000/a320 samples/modules/executorch/ethosu --sysbuild
   west build -t run

Building — XNNPACK CPU
***********************

.. code-block:: console

   west build -b fvp_corstone1000/a320 samples/modules/executorch/ethosu \
       --sysbuild -- -Dethosu_CONFIG_EXECUTORCH_BUILD_XNNPACK=y \
       -Dethosu_CONFIG_ETHOS_U=n \
       -Dethosu_ET_PTE_FILE_PATH=$(pwd)/samples/modules/executorch/ethosu/src/models/add_xnnpack.pte
   west build -t run

Custom model
************

To use a different ``.pte`` model file:

.. code-block:: console

   west build -b fvp_corstone1000/a320 samples/modules/executorch/ethosu \
       --sysbuild -- -Dethosu_ET_PTE_FILE_PATH=/path/to/model.pte

Pre-built models
****************

The ``src/models/`` directory contains three test models (simple
``add(x, x)`` with input ``[1, 1, 1, 1, 1]``, expected output
``[2, 2, 2, 2, 2]``):

- ``add_arm_delegate_ethos-u85-256.pte`` — Ethos-U85-256 NPU delegate
- ``add_xnnpack.pte`` — XNNPACK CPU backend (NEON optimized)
- ``add_cpu.pte`` — portable CPU operators (reference, no NEON)
