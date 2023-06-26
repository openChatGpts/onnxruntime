# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation.  All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
import logging
from typing import Union, List

from fusion_attention import AttentionMask
from onnx import TensorProto, GraphProto, helper
from onnx_model import OnnxModel
from onnx_model_bert import BertOnnxModel
from fusion_base import Fusion
from fusion_utils import FusionUtils
import numpy as np

logger = logging.getLogger(__name__)


class FusionTNLGV4Attention(Fusion):
    """
    Fuse GPT-2 Attention with past state subgraph into one Attention node.
    """

    def __init__(self, model: OnnxModel, num_heads: int):
        super().__init__(model, "Attention", ["MatMul"])
        self.num_heads = num_heads

    def create_attention_node(
        self,
        fc_weight,
        fc_bias,
        input,
        output,
        mask,
        past,
        present,
    ):
        attention_node_name = self.model.create_node_name("TNLGV4Attention")
        attention_node = helper.make_node(
            "Attention",
            inputs=[input, fc_weight, fc_bias, mask, past],
            outputs=[output, present],
            name=attention_node_name,
        )
        attention_node.domain = "com.microsoft"
        attention_node.attribute.extend(
            [
                helper.make_attribute("num_heads", self.num_heads),
                helper.make_attribute("unidirectional", 1),
                helper.make_attribute("do_rotary", 1),
                helper.make_attribute("scale", 0.0078125),
            ]
        )

        # attention_node.attribute.extend([helper.make_attribute("mask_filter_value", float(self.mask_filter_value))])

        self.nodes_to_add.extend([attention_node])
        self.node_name_to_graph_name[attention_node.name] = self.this_graph_name

        return attention_node

    def create_transpose_node(self, input_name: str, perm: List[int], output_name=None):
        """Append a Transpose node after an input"""
        node_name = self.model.create_node_name("Transpose")

        if output_name is None:
            output_name = node_name + "_out" + "-" + input_name

        transpose_node = helper.make_node("Transpose", inputs=[input_name], outputs=[output_name], name=node_name)
        transpose_node.attribute.extend([helper.make_attribute("perm", perm)])

        return transpose_node

    def fuse(self, node, input_name_to_nodes, output_name_to_node):
        # note: this is not an exact match. experiment purpose only.
        stem_nodes = self.model.match_parent_path(
            node,
            ['Reshape', 'Transpose', 'Reshape', 'MatMul', 'Reshape', 'Softmax', 'Where', 'Reshape', 'Add', 'Mul', 'MatMul']
        )
        if stem_nodes is None:
            return

        qk_matmul_node = stem_nodes[-1]

        qkv_matmul_nodes = self.model.match_parent_path(
            qk_matmul_node,
            ['Transpose', 'Reshape', 'Add', 'Mul', 'Split', 'Reshape', 'Add', 'MatMul'],
            [0, 0, 0, 0, 0, 0, 0, 1]
        )
        if qkv_matmul_nodes is None:
            return

        where_node = stem_nodes[-5]
        attn_mask_nodes = self.model.match_parent_path(
            where_node,
            ['Slice', 'Slice']
        )
        if attn_mask_nodes is None:
            return

        past_nodes = self.model.match_parent_path(
            qk_matmul_node,
            ['Transpose', 'Reshape', 'Concat', 'Squeeze', 'Split'],
            [1, 0, 0, 0, 0],
        )
        if past_nodes is None:
            return

        concat_node = past_nodes[-3]
        unsqueeze_node = self.model.find_first_child_by_type(concat_node, 'Unsqueeze')
        if unsqueeze_node is None:
            return
        concat_node_2 = self.model.find_first_child_by_type(unsqueeze_node, 'Concat')
        if concat_node_2 is None:
            return

        input = qkv_matmul_nodes[-1].input[0]
        attn_mask = attn_mask_nodes[-1].input[0]
        fc_weights = qkv_matmul_nodes[-1].input[1]
        fc_bias = qkv_matmul_nodes[-2].input[0]
        past = past_nodes[-1].input[0]
        output = node.input[0]
        present = concat_node_2.output[0]

        new_attn_node = self.create_attention_node(fc_weights, fc_bias, input, output, attn_mask, past, present)

        # Add a transpose node before/after the attention node
        transpose_before = self.create_transpose_node(new_attn_node.input[0], [1, 0, 2])
        new_attn_node.input[0] = transpose_before.output[0]
        self.model.add_node(transpose_before, self.this_graph_name)
        transpose_after = self.create_transpose_node(new_attn_node.output[0], [1, 0, 2])
        node.input[0] = transpose_after.output[0]
        self.model.add_node(transpose_after, self.this_graph_name)

        self.nodes_to_remove.extend(stem_nodes)
        self.nodes_to_remove.extend(qkv_matmul_nodes)
        self.nodes_to_remove.extend(attn_mask_nodes)
        self.nodes_to_remove.extend(past_nodes)
        self.nodes_to_remove.extend([concat_node_2])

        # todo_1: append transpose before and after the attention node
        self.prune_graph = True

def shape_of(vi):
    return tuple([d.dim_param if (d.dim_param) else d.dim_value for d in vi.type.tensor_type.shape.dim])

def change_io_shape(graph: GraphProto):
    new_inputs = []
    for i, vi in enumerate(graph.input):
        if vi.name == "attention_mask":
            vi = helper.make_tensor_value_info(
                vi.name,
                elem_type=TensorProto.INT32,
                shape=["batch_size", "seq_len"],
            )
        if "past" in vi.name:
            shape = shape_of(vi)
            vi = helper.make_tensor_value_info(
                vi.name,
                elem_type=vi.type.tensor_type.elem_type,
                shape=[shape[0], shape[2], shape[3], shape[1], shape[4]],
            )
        new_inputs.extend([vi])

    graph.ClearField("input")
    graph.input.extend(new_inputs)

    new_outputs = []
    for i, vi in enumerate(graph.output):
        if vi.name == "logits":
            vi = helper.make_tensor_value_info(
                vi.name,
                elem_type=TensorProto.FLOAT16,
                shape=shape_of(vi),
            )
        if "present" in vi.name:
            shape = shape_of(vi)
            vi = helper.make_tensor_value_info(
                vi.name,
                elem_type=vi.type.tensor_type.elem_type,
                shape=[shape[0], shape[2], shape[3], shape[1], shape[4]],
            )
        new_outputs.extend([vi])

    graph.ClearField("output")
    graph.output.extend(new_outputs)


class FusionBias(Fusion):
    def __init__(self, model: OnnxModel):
        super().__init__(model, "", "Add")

    def fuse(self, node, input_name_to_nodes, output_name_to_node):
        expand_nodes = self.model.match_parent_path(
            node,
            ['Expand', 'Shape'],
            [1, 1]
        )
        if expand_nodes is None:
            return

        expand_node = expand_nodes[0]
        node.input[1] = expand_node.input[0]
        self.nodes_to_remove.extend(expand_nodes)


class FusionTransposeRemover(Fusion):
    def __init__(self, model: OnnxModel):
        super().__init__(model, "", ["LayerNormalization", "SkipLayerNormalization", "Attention", "MatMul"])

    def fuse(self, node, input_name_to_nodes, output_name_to_node):
        transpose_nodes = self.model.match_parent_path(
            node,
            ['Transpose'],
            [0]
        )
        if transpose_nodes is None:
            return

        transpose_node = transpose_nodes[0]
        node.input[0] = transpose_node.input[0]
        #self.nodes_to_remove.extend(transpose_nodes)


class Tnlgv4OnnxModel(BertOnnxModel):
    def __init__(self, model, num_heads, hidden_size):
        super().__init__(model, num_heads, hidden_size)
        self.attention_mask = AttentionMask(self)
        self.attention_fusion = FusionTNLGV4Attention(self, self.num_heads)
        self.bias_fusion = FusionBias(self)
        self.transpose_remover = FusionTransposeRemover(self)

    def fuse_attention(self):
        self.attention_fusion.apply()

    def preprocess(self):
        self.utils.remove_useless_cast_nodes_in_fp16_model()

    def postprocess(self):
        self.bias_fusion.apply()
        self.transpose_remover.apply()
        self.fuse_skip_layer_norm()
        self.clean_graph()
        self.prune_graph()
        change_io_shape(self.model.graph)
