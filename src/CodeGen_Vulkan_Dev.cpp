#include <algorithm>
#include <sstream>
#include <unordered_set>

#include "CodeGen_GPU_Dev.h"
#include "CodeGen_Internal.h"
#include "CodeGen_Vulkan_Dev.h"
#include "Debug.h"
#include "Deinterleave.h"
#include "FindIntrinsics.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "Scope.h"
#include "SpirvIR.h"
#include "Target.h"

// Temporary:
#include <fstream>

#ifdef WITH_SPIRV

namespace Halide {
namespace Internal {

class CodeGen_LLVM;

namespace {  // anonymous

// --

template<typename CodeGenT, typename ValueT>
ValueT lower_int_uint_div(CodeGenT *cg, Expr a, Expr b);

template<typename CodeGenT, typename ValueT>
ValueT lower_int_uint_mod(CodeGenT *cg, Expr a, Expr b);

class CodeGen_Vulkan_Dev : public CodeGen_GPU_Dev {
public:
    CodeGen_Vulkan_Dev(Target target);

    /** Compile a GPU kernel into the module. This may be called many times
     * with different kernels, which will all be accumulated into a single
     * source module shared by a given Halide pipeline. */
    void add_kernel(Stmt stmt,
                    const std::string &name,
                    const std::vector<DeviceArgument> &args) override;

    /** (Re)initialize the GPU kernel module. This is separate from compile,
     * since a GPU device module will often have many kernels compiled into it
     * for a single pipeline. */
    void init_module() override;

    std::vector<char> compile_to_src() override;

    std::string get_current_kernel_name() override;

    void dump() override;

    std::string print_gpu_name(const std::string &name) override;

    std::string api_unique_name() override {
        return "vulkan";
    }

protected:
    class SPIRV_Emitter : public IRVisitor {

    public:
        SPIRV_Emitter() = default;

        using IRVisitor::visit;

        void visit(const IntImm *) override;
        void visit(const UIntImm *) override;
        void visit(const FloatImm *) override;
        void visit(const StringImm *) override;
        void visit(const Cast *) override;
        void visit(const Reinterpret *) override;
        void visit(const Variable *) override;
        void visit(const Add *) override;
        void visit(const Sub *) override;
        void visit(const Mul *) override;
        void visit(const Div *) override;
        void visit(const Mod *) override;
        void visit(const Min *) override;
        void visit(const Max *) override;
        void visit(const EQ *) override;
        void visit(const NE *) override;
        void visit(const LT *) override;
        void visit(const LE *) override;
        void visit(const GT *) override;
        void visit(const GE *) override;
        void visit(const And *) override;
        void visit(const Or *) override;
        void visit(const Not *) override;
        void visit(const Select *) override;
        void visit(const Load *) override;
        void visit(const Ramp *) override;
        void visit(const Broadcast *) override;
        void visit(const Call *) override;
        void visit(const Let *) override;
        void visit(const LetStmt *) override;
        void visit(const AssertStmt *) override;
        //        void visit(const ProducerConsumer *) override;
        void visit(const For *) override;
        void visit(const Store *) override;
        void visit(const Provide *) override;
        void visit(const Allocate *) override;
        void visit(const Free *) override;
        void visit(const Realize *) override;
        void visit(const IfThenElse *) override;
        void visit(const Evaluate *) override;
        void visit(const Shuffle *) override;
        void visit(const VectorReduce *) override;
        void visit(const Prefetch *) override;
        void visit(const Fork *) override;
        void visit(const Acquire *) override;
        void visit(const Atomic *) override;

        void visit_unaryop(Type t, const Expr &a, SpvOp op_code);
        void visit_binop(Type t, const Expr &a, const Expr &b, SpvOp op_code);

        void visit_glsl_unaryop(Type t, const Expr &a, SpvId glsl_op_code);
        void visit_glsl_binop(Type t, const Expr &a, const Expr &b, SpvId glsl_op_code);

        void load_from_scalar_index(SpvId index_id, SpvId base_id, SpvId type_id, SpvId ptr_type_id, SpvStorageClass storage_class);
        void load_from_vector_index(const Load *op, SpvId base_id, SpvId type_id, SpvId ptr_type_id, SpvStorageClass storage_class);

        void store_at_scalar_index(SpvId index_id, SpvId base_id, SpvId type_id, SpvId ptr_type_id, SpvStorageClass storage_class, SpvId value_id);
        void store_at_vector_index(const Store *op, SpvId base_id, SpvId type_id, SpvId ptr_type_id, SpvStorageClass storage_class, SpvId value_id);

        using BuiltinMap = std::unordered_map<std::string, SpvId>;
        const BuiltinMap spirv_builtin = {
            {"is_nan_f16", SpvOpIsNan},
            {"is_nan_f32", SpvOpIsNan},
            {"is_nan_f64", SpvOpIsNan},
            {"is_inf_f16", SpvOpIsInf},
            {"is_inf_f32", SpvOpIsInf},
            {"is_inf_f64", SpvOpIsInf},
        };

        const BuiltinMap glsl_builtin = {
            {"acos_f16", GLSLstd450Acos},
            {"acos_f32", GLSLstd450Acos},
            {"acosh_f16", GLSLstd450Acosh},
            {"acosh_f32", GLSLstd450Acosh},
            {"asin_f16", GLSLstd450Asin},
            {"asin_f32", GLSLstd450Asin},
            {"asinh_f16", GLSLstd450Asinh},
            {"asinh_f32", GLSLstd450Asinh},
            {"atan2_f16", GLSLstd450Atan},  // also called atan in GLSL
            {"atan2_f32", GLSLstd450Atan},
            {"atan_f16", GLSLstd450Atan},
            {"atan_f32", GLSLstd450Atan},
            {"atanh_f16", GLSLstd450Atanh},
            {"atanh_f32", GLSLstd450Atanh},
            {"ceil_f16", GLSLstd450Ceil},
            {"ceil_f32", GLSLstd450Ceil},
            {"cos_f16", GLSLstd450Cos},
            {"cos_f32", GLSLstd450Cos},
            {"cosh_f16", GLSLstd450Cosh},
            {"cosh_f32", GLSLstd450Cosh},
            {"exp_f16", GLSLstd450Exp},
            {"exp_f32", GLSLstd450Exp},
            {"fast_inverse_sqrt_f16", GLSLstd450InverseSqrt},
            {"fast_inverse_sqrt_f32", GLSLstd450InverseSqrt},
            {"floor_f16", GLSLstd450Floor},
            {"floor_f32", GLSLstd450Floor},
            {"log_f16", GLSLstd450Log},
            {"log_f32", GLSLstd450Log},
            {"sin_f16", GLSLstd450Sin},
            {"sin_f32", GLSLstd450Sin},
            {"sinh_f16", GLSLstd450Sinh},
            {"sinh_f32", GLSLstd450Sinh},
            {"sqrt_f16", GLSLstd450Sqrt},
            {"sqrt_f32", GLSLstd450Sqrt},
            {"tan_f16", GLSLstd450Tan},
            {"tan_f32", GLSLstd450Tan},
            {"tanh_f16", GLSLstd450Tanh},
            {"tanh_f32", GLSLstd450Tanh},
            {"trunc_f16", GLSLstd450Trunc},
            {"trunc_f32", GLSLstd450Trunc},
        };

        // The SPIRV-IR builder
        SpvBuilder builder;

        // Top-level function for adding kernels
        void add_kernel(const Stmt &s, const std::string &name, const std::vector<DeviceArgument> &args);
        void init_module();
        void compile(std::vector<char> &binary);
        void dump() const;

        // Scalarize expressions
        void scalarize(const Expr &e);
        SpvId map_type_to_pair(const Type &t);

        // Workgroup size
        void reset_workgroup_size();
        void declare_workgroup_size(SpvId kernel_func_id);
        void declare_entry_point(const Stmt &s, SpvId kernel_func_id);
        void declare_device_args(uint32_t entry_point_index, const std::string &kernel_name, const std::vector<DeviceArgument> &args);

        // The scope contains both the symbol id and its storage class
        using SymbolIdStorageClassPair = std::pair<SpvId, SpvStorageClass>;
        using SymbolScope = Scope<SymbolIdStorageClassPair>;
        using ScopedSymbolBinding = ScopedBinding<SymbolIdStorageClassPair>;
        SymbolScope symbol_table;

        // Keep track of the descriptor sets so we can add a sidecar to module
        // indicating which descriptor set to use for each entry point
        struct DescriptorSet {
            std::string entry_point_name;
            uint32_t uniform_buffer_count = 0;
            uint32_t storage_buffer_count = 0;
        };
        using DescriptorSetTable = std::vector<DescriptorSet>;
        DescriptorSetTable descriptor_set_table;

        // Encode the descriptor sets into a sidecar which will be added
        // as a header to the module prior to the actual SPIR-V binary
        void encode_header(SpvBinary &spirv_header);

        // The workgroup size.  May vary between kernels.
        uint32_t workgroup_size[3];

        // Returns Phi node inputs.
        template<typename StmtOrExpr>
        SpvFactory::BlockVariables emit_if_then_else(const Expr &condition, StmtOrExpr then_case, StmtOrExpr else_case);

    } emitter;

    std::string current_kernel_name;
};

void CodeGen_Vulkan_Dev::SPIRV_Emitter::scalarize(const Expr &e) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::scalarize(): " << (Expr)e << "\n";
    internal_assert(e.type().is_vector()) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::scalarize must be called with an expression of vector type.\n";

    SpvId type_id = builder.declare_type(e.type());
    SpvId value_id = builder.declare_null_constant(e.type());
    SpvId result_id = value_id;
    for (int i = 0; i < e.type().lanes(); i++) {
        extract_lane(e, i).accept(this);
        SpvId vector_id = builder.current_id();
        SpvId composite_vector_id = builder.reserve_id(SpvResultId);
        builder.append(SpvFactory::vector_insert_dynamic(type_id, composite_vector_id, vector_id, value_id, i));
        result_id = composite_vector_id;
    }
    builder.update_id(result_id);
}

SpvId CodeGen_Vulkan_Dev::SPIRV_Emitter::map_type_to_pair(const Type &t) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::map_type_to_pair(): " << t << "\n";
    SpvId base_type_id = builder.declare_type(t);
    SpvBuilder::StructMemberTypes member_type_ids = {base_type_id, base_type_id};
    const std::string struct_name = std::string("_struct_") + type_to_c_type(t, false, false) + std::string("_pair");
    SpvId struct_type_id = builder.declare_struct(struct_name, member_type_ids);
    return struct_type_id;
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Variable *var) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Variable): " << var->type << " " << var->name << "\n";
    SpvId variable_id = symbol_table.get(var->name).first;
    user_assert(variable_id != SpvInvalidId) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Variable): Invalid symbol name!\n";
    builder.update_id(variable_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const IntImm *imm) {
    if (imm->type.bits() == 8) {
        const int8_t value = (int8_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else if (imm->type.bits() == 16) {
        const int16_t value = (int16_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else if (imm->type.bits() == 32) {
        const int32_t value = (int32_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else if (imm->type.bits() == 64) {
        const int64_t value = (int64_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else {
        internal_error << "Vulkan backend currently only supports 8-bit, 16-bit, 32-bit or 64-bit signed integers!\n";
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const UIntImm *imm) {
    if (imm->type.bits() == 8) {
        const uint8_t value = (uint8_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else if (imm->type.bits() == 16) {
        const uint16_t value = (uint16_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else if (imm->type.bits() == 32) {
        const uint32_t value = (uint32_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else if (imm->type.bits() == 64) {
        const uint64_t value = (uint64_t)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else {
        internal_error << "Vulkan backend currently only supports 8-bit, 16-bit, 32-bit or 64-bit unsigned integers!\n";
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const StringImm *imm) {
    SpvId constant_id = builder.declare_string_constant(imm->value);
    builder.update_id(constant_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const FloatImm *imm) {
    if (imm->type.bits() == 32) {
        const float value = (float)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else if (imm->type.bits() == 64) {
        const double value = (double)(imm->value);
        SpvId constant_id = builder.declare_constant(imm->type, &value);
        builder.update_id(constant_id);
    } else {
        internal_error << "Vulkan backend currently only supports 32-bit or 64-bit floats\n";
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Cast *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Cast): " << op->value.type() << " to " << op->type << "\n";

    Type value_type = op->value.type();
    Type target_type = op->type;
    SpvId target_type_id = builder.declare_type(target_type);
    op->value.accept(this);
    SpvId src_id = builder.current_id();

    SpvOp op_code = SpvOpNop;
    if (value_type.is_float()) {
        if (target_type.is_float()) {
            op_code = SpvOpFConvert;
        } else if (target_type.is_uint()) {
            op_code = SpvOpConvertFToU;
        } else if (target_type.is_int()) {
            op_code = SpvOpConvertFToS;
        } else {
            internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Cast):  unhandled case " << value_type << " to " << target_type << "\n";
        }
    } else if (value_type.is_uint()) {
        if (target_type.is_float()) {
            op_code = SpvOpConvertUToF;
        } else if (target_type.is_uint()) {
            op_code = SpvOpUConvert;
        } else if (target_type.is_int()) {
            if (builder.is_capability_required(SpvCapabilityKernel)) {
                op_code = SpvOpSatConvertUToS;
            } else {
                op_code = SpvOpBitcast;
            }
        } else {
            internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Cast):  unhandled case " << value_type << " to " << target_type << "\n";
        }
    } else if (value_type.is_int()) {
        if (target_type.is_float()) {
            op_code = SpvOpConvertSToF;
        } else if (target_type.is_uint()) {
            if (builder.is_capability_required(SpvCapabilityKernel)) {
                op_code = SpvOpSatConvertSToU;
            } else {
                op_code = SpvOpBitcast;
            }
        } else if (target_type.is_int()) {
            op_code = SpvOpSConvert;
        } else {
            internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Cast):  unhandled case " << value_type << " to " << target_type << "\n";
        }
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Cast):  unhandled case " << value_type << " to " << target_type << "\n";
    }

    SpvId result_id = builder.reserve_id(SpvResultId);
    if (op_code == SpvOpBitcast) {
        builder.append(SpvFactory::bitcast(target_type_id, result_id, src_id));
    } else {
        builder.append(SpvFactory::convert(op_code, target_type_id, result_id, src_id));
    }
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Reinterpret *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Reinterpret): " << op->value.type() << " to " << op->type << "\n";
    SpvId type_id = builder.declare_type(op->type);
    op->value.accept(this);
    SpvId src_id = builder.current_id();
    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::bitcast(type_id, result_id, src_id));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Add *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Add): " << op->type << " ((" << op->a << ") + (" << op->b << "))\n";
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFAdd : SpvOpIAdd);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Sub *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Sub): " << op->type << " ((" << op->a << ") - (" << op->b << "))\n";
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFSub : SpvOpISub);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Mul *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Mul): " << op->type << " ((" << op->a << ") * (" << op->b << "))\n";
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFMul : SpvOpIMul);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Div *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Div): " << op->type << " ((" << op->a << ") / (" << op->b << "))\n";
    user_assert(!is_const_zero(op->b)) << "Division by constant zero in expression: " << Expr(op) << "\n";

    if (op->type.is_float()) {
        visit_binop(op->type, op->a, op->b, SpvOpFDiv);
    } else {
        Expr e = lower_int_uint_div(op->a, op->b);
        e.accept(this);
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Mod *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Mod): " << op->type << " ((" << op->a << ") % (" << op->b << "))\n";
    if (op->type.is_float()) {
        // Takes sign of result from op->b
        visit_binop(op->type, op->a, op->b, SpvOpFMod);
    } else {
        Expr e = lower_int_uint_mod(op->a, op->b);
        e.accept(this);
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Max *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Max): " << op->type << " Max((" << op->a << "), (" << op->b << "))\n";

    std::string a_name = unique_name('a');
    std::string b_name = unique_name('b');
    Expr a = Variable::make(op->a.type(), a_name);
    Expr b = Variable::make(op->b.type(), b_name);
    Expr temp = Let::make(a_name, op->a,
                          Let::make(b_name, op->b, select(a > b, a, b)));
    temp.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Min *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Min): " << op->type << " Min((" << op->a << "), (" << op->b << "))\n";
    std::string a_name = unique_name('a');
    std::string b_name = unique_name('b');
    Expr a = Variable::make(op->a.type(), a_name);
    Expr b = Variable::make(op->b.type(), b_name);
    Expr temp = Let::make(a_name, op->a,
                          Let::make(b_name, op->b, select(a < b, a, b)));
    temp.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const EQ *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(EQ): " << op->type << " (" << op->a << ") == (" << op->b << ")\n";
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFOrdEqual : SpvOpIEqual);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const NE *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(NE): " << op->type << " (" << op->a << ") != (" << op->b << ")\n";
    visit_binop(op->type, op->a, op->b, op->type.is_float() ? SpvOpFOrdNotEqual : SpvOpINotEqual);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const LT *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(LT): " << op->type << " (" << op->a << ") < (" << op->b << ")\n";
    SpvOp op_code = SpvOpNop;
    if (op->a.type().is_float()) {
        op_code = SpvOpFOrdLessThan;
    } else if (op->a.type().is_int()) {
        op_code = SpvOpSLessThan;
    } else if (op->a.type().is_uint()) {
        op_code = SpvOpULessThan;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const LT *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, op_code);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const LE *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(LE): " << op->type << " (" << op->a << ") <= (" << op->b << ")\n";
    SpvOp op_code = SpvOpNop;
    if (op->a.type().is_float()) {
        op_code = SpvOpFOrdLessThanEqual;
    } else if (op->a.type().is_int()) {
        op_code = SpvOpSLessThanEqual;
    } else if (op->a.type().is_uint()) {
        op_code = SpvOpULessThanEqual;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const LE *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, op_code);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const GT *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(GT): " << op->type << " (" << op->a << ") > (" << op->b << ")\n";
    SpvOp op_code = SpvOpNop;
    if (op->a.type().is_float()) {
        op_code = SpvOpFOrdGreaterThan;
    } else if (op->a.type().is_int()) {
        op_code = SpvOpSGreaterThan;
    } else if (op->a.type().is_uint()) {
        op_code = SpvOpUGreaterThan;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const GT *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, op_code);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const GE *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(GE): " << op->type << " (" << op->a << ") >= (" << op->b << ")\n";
    SpvOp op_code = SpvOpNop;
    if (op->a.type().is_float()) {
        op_code = SpvOpFOrdGreaterThanEqual;
    } else if (op->a.type().is_int()) {
        op_code = SpvOpSGreaterThanEqual;
    } else if (op->a.type().is_uint()) {
        op_code = SpvOpUGreaterThanEqual;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const GE *op): unhandled type: " << op->a.type() << "\n";
    }
    visit_binop(op->type, op->a, op->b, op_code);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const And *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(And): " << op->type << " (" << op->a << ") && (" << op->b << ")\n";
    visit_binop(op->type, op->a, op->b, SpvOpLogicalAnd);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Or *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Or): " << op->type << " (" << op->a << ") || (" << op->b << ")\n";
    visit_binop(op->type, op->a, op->b, SpvOpLogicalOr);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Not *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Not): " << op->type << " !(" << op->a << ")\n";

    SpvId type_id = builder.declare_type(op->type);
    op->a.accept(this);
    SpvId src_id = builder.current_id();
    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::logical_not(type_id, result_id, src_id));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Call *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Call): " << op->type << " " << op->name << " args=" << (uint32_t)op->args.size() << "\n";

    if (op->is_intrinsic(Call::gpu_thread_barrier)) {
        internal_assert(op->args.size() == 1) << "gpu_thread_barrier() intrinsic must specify memory fence type.\n";

        const auto *fence_type_ptr = as_const_int(op->args[0]);
        internal_assert(fence_type_ptr) << "gpu_thread_barrier() parameter is not a constant integer.\n";
        auto fence_type = *fence_type_ptr;

        if (fence_type & CodeGen_GPU_Dev::MemoryFenceType::Device &&
            !(fence_type & CodeGen_GPU_Dev::MemoryFenceType::Shared)) {

            uint32_t execution_scope = SpvDeviceScope;
            uint32_t memory_scope = SpvDeviceScope;
            uint32_t control_mask = (SpvMemorySemanticsAcquireReleaseMask |
                                     SpvMemorySemanticsWorkgroupMemoryMask);
            SpvId exec_scope_id = builder.declare_constant(UInt(32), &execution_scope);
            SpvId memory_scope_id = builder.declare_constant(UInt(32), &memory_scope);
            SpvId control_mask_id = builder.declare_constant(UInt(32), &control_mask);
            builder.append(SpvFactory::control_barrier(exec_scope_id, memory_scope_id, control_mask_id));

        } else if (fence_type & CodeGen_GPU_Dev::MemoryFenceType::Device) {
            uint32_t execution_scope = SpvDeviceScope;
            uint32_t memory_scope = SpvDeviceScope;
            uint32_t control_mask = (SpvMemorySemanticsSequentiallyConsistentMask |
                                     SpvMemorySemanticsUniformMemoryMask |
                                     SpvMemorySemanticsSubgroupMemoryMask |
                                     SpvMemorySemanticsWorkgroupMemoryMask |
                                     SpvMemorySemanticsCrossWorkgroupMemoryMask |
                                     SpvMemorySemanticsAtomicCounterMemoryMask |
                                     SpvMemorySemanticsImageMemoryMask);
            SpvId exec_scope_id = builder.declare_constant(UInt(32), &execution_scope);
            SpvId memory_scope_id = builder.declare_constant(UInt(32), &memory_scope);
            SpvId control_mask_id = builder.declare_constant(UInt(32), &control_mask);
            builder.append(SpvFactory::control_barrier(exec_scope_id, memory_scope_id, control_mask_id));

        } else if (fence_type & CodeGen_GPU_Dev::MemoryFenceType::Shared) {
            uint32_t memory_scope = SpvWorkgroupScope;
            uint32_t control_mask = (SpvMemorySemanticsAcquireReleaseMask |
                                     SpvMemorySemanticsWorkgroupMemoryMask);
            SpvId memory_scope_id = builder.declare_constant(UInt(32), &memory_scope);
            SpvId control_mask_id = builder.declare_constant(UInt(32), &control_mask);
            builder.append(SpvFactory::memory_barrier(memory_scope_id, control_mask_id));

        } else {

            uint32_t execution_scope = SpvDeviceScope;
            uint32_t memory_scope = SpvDeviceScope;
            uint32_t control_mask = SpvMemorySemanticsMaskNone;
            SpvId exec_scope_id = builder.declare_constant(UInt(32), &execution_scope);
            SpvId memory_scope_id = builder.declare_constant(UInt(32), &memory_scope);
            SpvId control_mask_id = builder.declare_constant(UInt(32), &control_mask);
            builder.append(SpvFactory::control_barrier(exec_scope_id, memory_scope_id, control_mask_id));
        }
    } else if (op->is_intrinsic(Call::abs)) {
        internal_assert(op->args.size() == 1);

        SpvId op_code = SpvInvalidId;
        if (op->type.is_float()) {
            op_code = GLSLstd450FAbs;
        } else {
            op_code = GLSLstd450SAbs;
        }
        visit_glsl_unaryop(op->type, op->args[0], op_code);

    } else if (op->is_intrinsic(Call::IntrinsicOp::round)) {
        internal_assert(op->args.size() == 1);
        visit_glsl_unaryop(op->type, op->args[0], GLSLstd450RoundEven);

    } else if (op->is_intrinsic(Call::absd)) {
        internal_assert(op->args.size() == 2);
        Expr a = op->args[0];
        Expr b = op->args[1];
        Expr e = cast(op->type, select(a < b, b - a, a - b));
        e->accept(this);
    } else if (op->is_intrinsic(Call::return_second)) {
        internal_assert(op->args.size() == 2);
        // Simply discard the first argument, which is generally a call to
        // 'halide_printf'.
        if (op->args[1].defined()) {
            op->args[1]->accept(this);
        }
    } else if (op->is_intrinsic(Call::bitwise_and)) {
        internal_assert(op->args.size() == 2);
        visit_binop(op->type, op->args[0], op->args[1], SpvOpBitwiseAnd);
    } else if (op->is_intrinsic(Call::bitwise_xor)) {
        internal_assert(op->args.size() == 2);
        visit_binop(op->type, op->args[0], op->args[1], SpvOpBitwiseXor);
    } else if (op->is_intrinsic(Call::bitwise_or)) {
        internal_assert(op->args.size() == 2);
        visit_binop(op->type, op->args[0], op->args[1], SpvOpBitwiseOr);
    } else if (op->is_intrinsic(Call::bitwise_not)) {
        internal_assert(op->args.size() == 1);
        SpvId type_id = builder.declare_type(op->type);
        op->args[0]->accept(this);
        SpvId arg_id = builder.current_id();
        SpvId result_id = builder.reserve_id(SpvResultId);
        builder.append(SpvFactory::bitwise_not(type_id, result_id, arg_id));
        builder.update_id(result_id);
    } else if (op->is_intrinsic(Call::if_then_else)) {
        if (op->type.is_vector()) {
            scalarize(op);
        } else {
            // Generate Phi node if used as an expression.
            internal_assert(op->args.size() == 3);
            SpvFactory::BlockVariables block_vars = emit_if_then_else(op->args[0], op->args[1], op->args[2]);
            SpvId type_id = builder.declare_type(op->type);
            SpvId result_id = builder.reserve_id(SpvResultId);
            builder.append(SpvFactory::phi(type_id, result_id, block_vars));
            builder.update_id(result_id);
        }
    } else if (op->is_intrinsic(Call::IntrinsicOp::div_round_to_zero)) {
        internal_assert(op->args.size() == 2);
        SpvOp op_code = SpvOpNop;
        if (op->type.is_float()) {
            op_code = SpvOpFDiv;
        } else if (op->type.is_int()) {
            op_code = SpvOpSDiv;
        } else if (op->type.is_uint()) {
            op_code = SpvOpUDiv;
        } else {
            internal_error << "div_round_to_zero of unhandled type.\n";
        }
        visit_binop(op->type, op->args[0], op->args[1], op_code);
    } else if (op->is_intrinsic(Call::IntrinsicOp::mod_round_to_zero)) {
        internal_assert(op->args.size() == 2);
        SpvOp op_code = SpvOpNop;
        if (op->type.is_float()) {
            op_code = SpvOpFMod;
        } else if (op->type.is_int()) {
            op_code = SpvOpSMod;
        } else if (op->type.is_uint()) {
            op_code = SpvOpUMod;
        } else {
            internal_error << "mod_round_to_zero of unhandled type.\n";
        }
        visit_binop(op->type, op->args[0], op->args[1], op_code);

    } else if (op->is_intrinsic(Call::shift_right)) {
        if (op->type.is_uint()) {
            visit_binop(op->type, op->args[0], op->args[1], SpvOpShiftRightLogical);
        } else {
            visit_binop(op->type, op->args[0], op->args[1], SpvOpShiftRightArithmetic);
        }
    } else if (op->is_intrinsic(Call::shift_left)) {
        visit_binop(op->type, op->args[0], op->args[1], SpvOpShiftLeftLogical);
    } else if (op->is_intrinsic(Call::strict_float)) {
        // TODO: Enable/Disable RelaxedPrecision flags?
        internal_assert(op->args.size() == 1);
        op->args[0].accept(this);
    } else if (op->is_intrinsic(Call::IntrinsicOp::sorted_avg)) {
        internal_assert(op->args.size() == 2);
        // b > a, so the following works without widening:
        // a + (b - a)/2
        Expr e = op->args[0] + (op->args[1] - op->args[0]) / 2;
        e.accept(this);

    } else if (op->is_intrinsic(Call::widen_right_add) ||
               op->is_intrinsic(Call::widen_right_mul) ||
               op->is_intrinsic(Call::widen_right_sub) ||
               op->is_intrinsic(Call::widening_add) ||
               op->is_intrinsic(Call::widening_mul) ||
               op->is_intrinsic(Call::widening_sub) ||
               op->is_intrinsic(Call::widening_shift_left) ||
               op->is_intrinsic(Call::widening_shift_right) ||
               op->is_intrinsic(Call::rounding_shift_left) ||
               op->is_intrinsic(Call::rounding_shift_right) ||
               op->is_intrinsic(Call::saturating_cast) ||
               op->is_intrinsic(Call::saturating_add) ||
               op->is_intrinsic(Call::saturating_sub) ||
               op->is_intrinsic(Call::saturating_cast) ||
               op->is_intrinsic(Call::halving_add) ||
               op->is_intrinsic(Call::halving_sub) ||
               op->is_intrinsic(Call::rounding_halving_add) ||
               op->is_intrinsic(Call::mul_shift_right) ||
               op->is_intrinsic(Call::rounding_mul_shift_right)) {
        Expr e = lower_intrinsic(op);
        e.accept(this);
        return;
    } else {

        // First check for a standard SPIR-V built-in
        BuiltinMap::const_iterator spirv_it = spirv_builtin.find(op->name);
        if (spirv_it != spirv_builtin.end()) {
            SpvId spirv_op = spirv_it->second;
            if ((spirv_op == SpvOpIsInf) || (spirv_op == SpvOpIsNan)) {
                internal_assert(op->args.size() == 1);
                visit_unaryop(op->type, op->args[0], (SpvOp)spirv_op);
            } else {
                user_error << "Vulkan: unhandled SPIR-V builtin function '" << op->name << "' encountered.\n";
            }
        }

        // If its not a standard SPIR-V built-in, see if there's a GLSL extended builtin
        BuiltinMap::const_iterator glsl_it = glsl_builtin.find(op->name);
        if (glsl_it == glsl_builtin.end()) {
            user_error << "Vulkan: unknown function '" << op->name << "' encountered.\n";
        }

        SpvId glsl_op_code = glsl_it->second;
        if (is_glsl_unary_op(glsl_op_code)) {
            internal_assert(op->args.size() == 1);
            visit_glsl_unaryop(op->type, op->args[0], glsl_op_code);
        } else if (is_glsl_binary_op(glsl_op_code)) {
            internal_assert(op->args.size() == 2);
            visit_glsl_binop(op->type, op->args[0], op->args[1], glsl_op_code);
        } else {
            user_error << "Vulkan: unhandled SPIR-V GLSL builtin function '" << op->name << "' encountered.\n";
        }
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Select *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Select): " << op->type << " (" << op->condition << ") ? (" << op->true_value << ") : (" << op->false_value << ")\n";
    SpvId type_id = builder.declare_type(op->type);
    op->condition.accept(this);
    SpvId cond_id = builder.current_id();
    op->true_value.accept(this);
    SpvId true_id = builder.current_id();
    op->false_value.accept(this);
    SpvId false_id = builder.current_id();
    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::select(type_id, result_id, cond_id, true_id, false_id));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::load_from_scalar_index(SpvId index_id, SpvId base_id, SpvId result_type_id, SpvId ptr_type_id, SpvStorageClass storage_class) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::load_from_scalar_index(): "
             << "index_id=" << index_id << " "
             << "base_id=" << base_id << " "
             << "ptr_type_id=" << ptr_type_id << " "
             << "result_type_id=" << result_type_id << " "
             << "storage_class=" << storage_class << "\n";

    // determine the base type id for the source value
    SpvId base_type_id = builder.type_of(base_id);
    if (builder.is_pointer_type(base_type_id)) {
        base_type_id = builder.lookup_base_type(base_type_id);
    }

    uint32_t zero = 0;
    SpvId src_id = SpvInvalidId;
    SpvId src_index_id = index_id;
    if (storage_class == SpvStorageClassUniform) {
        if (builder.is_struct_type(base_type_id)) {
            SpvId zero_id = builder.declare_constant(UInt(32), &zero);
            SpvFactory::Indices access_indices = {zero_id, src_index_id};
            src_id = builder.declare_access_chain(ptr_type_id, base_id, access_indices);
        } else {
            SpvFactory::Indices access_indices = {src_index_id};
            src_id = builder.declare_access_chain(ptr_type_id, base_id, access_indices);
        }
    } else if (storage_class == SpvStorageClassWorkgroup) {
        SpvFactory::Indices access_indices = {src_index_id};
        src_id = builder.declare_access_chain(ptr_type_id, base_id, access_indices);
    } else if (storage_class == SpvStorageClassFunction) {
        src_id = base_id;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Load): unhandled storage class encountered on op: " << storage_class << "\n";
    }
    internal_assert(src_id != SpvInvalidId);

    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::load(result_type_id, result_id, src_id));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::load_from_vector_index(const Load *op, SpvId base_id, SpvId type_id, SpvId ptr_type_id, SpvStorageClass storage_class) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::load_from_vector_index(): "
             << "base_id=" << base_id << " "
             << "type_id=" << type_id << " "
             << "ptr_type_id=" << ptr_type_id << " "
             << "storage_class=" << storage_class << "\n";

    internal_assert(op->index.type().is_vector());

    // determine the base type id for the source value
    SpvId base_type_id = builder.type_of(base_id);
    if (builder.is_pointer_type(base_type_id)) {
        base_type_id = builder.lookup_base_type(base_type_id);
    }

    // If this is a dense vector load and the buffer has a vector base type,
    // then index the buffer using the base of the ramp divided by the number
    // of lanes.
    if (builder.is_vector_type(base_type_id)) {
        Expr ramp_base = strided_ramp_base(op->index);
        if (ramp_base.defined()) {
            Expr ramp_index = (ramp_base / op->type.lanes());
            ramp_index.accept(this);
            SpvId index_id = builder.current_id();
            load_from_scalar_index(index_id, base_id, type_id, ptr_type_id, storage_class);
            return;
        }
    }

    op->index.accept(this);
    SpvId index_id = builder.current_id();

    // Gather vector elements.
    SpvFactory::Components loaded_values;
    SpvId scalar_value_type_id = builder.declare_type(op->type.with_lanes(1));
    SpvId scalar_ptr_type_id = builder.declare_pointer_type(scalar_value_type_id, storage_class);
    SpvId scalar_index_type_id = builder.declare_type(op->index.type().with_lanes(1));
    for (uint32_t i = 0; i < (uint32_t)op->index.type().lanes(); i++) {
        SpvFactory::Indices extract_indices = {i};
        SpvId index_component_id = builder.reserve_id(SpvResultId);
        builder.append(SpvFactory::composite_extract(scalar_index_type_id, index_component_id, index_id, extract_indices));
        load_from_scalar_index(index_component_id, base_id, scalar_value_type_id, scalar_ptr_type_id, storage_class);
        SpvId value_component_id = builder.current_id();
        loaded_values.push_back(value_component_id);
    }

    // Create a composite vector from the individual loads
    if (loaded_values.size() > 1) {
        SpvId result_id = builder.reserve_id(SpvResultId);
        builder.append(SpvFactory::composite_construct(type_id, result_id, loaded_values));
        builder.update_id(result_id);
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::store_at_scalar_index(SpvId index_id, SpvId base_id, SpvId type_id, SpvId ptr_type_id, SpvStorageClass storage_class, SpvId value_id) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::store_at_scalar_index(): "
             << "index_id=" << index_id << " "
             << "base_id=" << base_id << " "
             << "type_id=" << type_id << " "
             << "ptr_type_id=" << ptr_type_id << " "
             << "storage_class=" << storage_class << " "
             << "value_id=" << value_id << "\n";

    // determine the base type id for the source value
    SpvId base_type_id = builder.type_of(base_id);
    if (builder.is_pointer_type(base_type_id)) {
        base_type_id = builder.lookup_base_type(base_type_id);
    }

    uint32_t zero = 0;
    SpvId dst_id = SpvInvalidId;
    SpvId dst_index_id = index_id;

    if (storage_class == SpvStorageClassUniform) {
        if (builder.is_struct_type(base_type_id)) {
            SpvId zero_id = builder.declare_constant(UInt(32), &zero);
            SpvFactory::Indices access_indices = {zero_id, dst_index_id};
            dst_id = builder.declare_access_chain(ptr_type_id, base_id, access_indices);
        } else {
            SpvFactory::Indices access_indices = {dst_index_id};
            dst_id = builder.declare_access_chain(ptr_type_id, base_id, access_indices);
        }
    } else if (storage_class == SpvStorageClassWorkgroup) {
        SpvFactory::Indices access_indices = {dst_index_id};
        dst_id = builder.declare_access_chain(ptr_type_id, base_id, access_indices);
    } else if (storage_class == SpvStorageClassFunction) {
        dst_id = base_id;
    } else {
        internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Store): unhandled storage class encountered on op: " << storage_class << "\n";
    }
    internal_assert(dst_id != SpvInvalidId);
    builder.append(SpvFactory::store(dst_id, value_id));
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::store_at_vector_index(const Store *op, SpvId base_id, SpvId type_id, SpvId ptr_type_id, SpvStorageClass storage_class, SpvId value_id) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::store_at_vector_index(): "
             << "base_id=" << base_id << " "
             << "type_id=" << type_id << " "
             << "ptr_type_id=" << ptr_type_id << " "
             << "storage_class=" << storage_class << "\n";

    internal_assert(op->index.type().is_vector());

    // determine the base type id for the source value
    SpvId base_type_id = builder.type_of(base_id);
    if (builder.is_pointer_type(base_type_id)) {
        base_type_id = builder.lookup_base_type(base_type_id);
    }

    // If this is a dense vector load and the buffer has a vector base type,
    // then index the buffer using the base of the ramp divided by the number
    // of lanes.
    if (builder.is_vector_type(base_type_id)) {
        Expr ramp_base = strided_ramp_base(op->index);
        if (ramp_base.defined()) {
            Expr ramp_index = (ramp_base / op->value.type().lanes());
            ramp_index.accept(this);
            SpvId index_id = builder.current_id();
            store_at_scalar_index(index_id, base_id, type_id, ptr_type_id, storage_class, value_id);
            return;
        }
    }

    op->index.accept(this);
    SpvId index_id = builder.current_id();

    // Scatter vector elements.
    SpvId scalar_value_type_id = builder.declare_type(op->value.type().with_lanes(1));
    SpvId scalar_ptr_type_id = builder.declare_pointer_type(scalar_value_type_id, storage_class);
    SpvId scalar_index_type_id = builder.declare_type(op->index.type().with_lanes(1));
    for (uint32_t i = 0; i < (uint32_t)op->index.type().lanes(); i++) {
        SpvFactory::Indices extract_indices = {i};
        SpvId index_component_id = builder.reserve_id(SpvResultId);
        builder.append(SpvFactory::composite_extract(scalar_index_type_id, index_component_id, index_id, extract_indices));
        SpvId value_component_id = builder.reserve_id(SpvResultId);
        builder.append(SpvFactory::composite_extract(scalar_value_type_id, value_component_id, value_id, extract_indices));
        store_at_scalar_index(index_component_id, base_id, scalar_value_type_id, scalar_ptr_type_id, storage_class, value_component_id);
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Load *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Load): " << op->type << " " << op->name << "[" << op->index << "]\n";
    user_assert(is_const_one(op->predicate)) << "Predicated loads not supported by SPIR-V codegen\n";

    // Construct the pointer to read from
    internal_assert(symbol_table.contains(op->name));
    SymbolIdStorageClassPair id_and_storage_class = symbol_table.get(op->name);
    SpvId base_id = id_and_storage_class.first;
    SpvStorageClass storage_class = id_and_storage_class.second;
    internal_assert(base_id != SpvInvalidId);
    internal_assert(((uint32_t)storage_class) < ((uint32_t)SpvStorageClassMax));

    SpvId type_id = builder.declare_type(op->type);
    SpvId ptr_type_id = builder.declare_pointer_type(type_id, storage_class);

    if (op->index.type().is_scalar()) {
        op->index.accept(this);
        SpvId index_id = builder.current_id();
        load_from_scalar_index(index_id, base_id, type_id, ptr_type_id, storage_class);
    } else {

        // If this is a dense vector load and the buffer has a vector base type,
        // then index the buffer using the base of the ramp divided by the number
        // of lanes.
        if (op->type.is_vector()) {
            Expr ramp_base = strided_ramp_base(op->index);
            if (ramp_base.defined()) {
                Expr ramp_index = (ramp_base / op->type.lanes());
                ramp_index.accept(this);
                SpvId index_id = builder.current_id();
                load_from_scalar_index(index_id, base_id, type_id, ptr_type_id, storage_class);
            }
        }
        load_from_vector_index(op, base_id, type_id, ptr_type_id, storage_class);
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Store *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Store): " << op->name << "[" << op->index << "] = (" << op->value << ")\n";
    user_assert(is_const_one(op->predicate)) << "Predicated stores not supported by SPIR-V codegen!\n";

    op->value.accept(this);
    SpvId value_id = builder.current_id();

    // Construct the pointer to write to
    internal_assert(symbol_table.contains(op->name));
    SymbolIdStorageClassPair id_and_storage_class = symbol_table.get(op->name);
    SpvId base_id = id_and_storage_class.first;
    SpvStorageClass storage_class = id_and_storage_class.second;
    internal_assert(base_id != SpvInvalidId);
    internal_assert(((uint32_t)storage_class) < ((uint32_t)SpvStorageClassMax));

    SpvId type_id = builder.declare_type(op->value.type());
    SpvId ptr_type_id = builder.declare_pointer_type(type_id, storage_class);

    if (op->index.type().is_scalar()) {
        op->index.accept(this);
        SpvId index_id = builder.current_id();
        store_at_scalar_index(index_id, base_id, type_id, ptr_type_id, storage_class, value_id);
    } else {
        store_at_vector_index(op, base_id, type_id, ptr_type_id, storage_class, value_id);
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Let *let) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Let): " << (Expr)let << "\n";
    let->value.accept(this);
    SpvId current_id = builder.current_id();
    ScopedSymbolBinding binding(symbol_table, let->name, {current_id, SpvStorageClassFunction});
    let->body.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const LetStmt *let) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(LetStmt): " << let->name << "\n";
    let->value.accept(this);
    SpvId current_id = builder.current_id();
    ScopedSymbolBinding binding(symbol_table, let->name, {current_id, SpvStorageClassFunction});
    let->body.accept(this);

    // TODO: Figure out undef here?
    builder.update_id(SpvInvalidId);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const AssertStmt *stmt) {
    // TODO: Fill this in.
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(AssertStmt): "
             << "condition=" << stmt->condition << " "
             << "message=" << stmt->message << "\n";
}

namespace {
std::pair<std::string, uint32_t> simt_intrinsic(const std::string &name) {
    if (ends_with(name, ".__thread_id_x")) {
        return {"LocalInvocationId", 0};
    } else if (ends_with(name, ".__thread_id_y")) {
        return {"LocalInvocationId", 1};
    } else if (ends_with(name, ".__thread_id_z")) {
        return {"LocalInvocationId", 2};
    } else if (ends_with(name, ".__block_id_x")) {
        return {"WorkgroupId", 0};
    } else if (ends_with(name, ".__block_id_y")) {
        return {"WorkgroupId", 1};
    } else if (ends_with(name, ".__block_id_z")) {
        return {"WorkgroupId", 2};
    } else if (ends_with(name, "id_w")) {
        user_error << "Vulkan only supports <=3 dimensions for gpu blocks";
    }
    internal_error << "simt_intrinsic called on bad variable name: " << name << "\n";
    return {"", -1};
}

int thread_loop_workgroup_index(const std::string &name) {
    std::string ids[] = {".__thread_id_x",
                         ".__thread_id_y",
                         ".__thread_id_z"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(std::string); i++) {
        if (ends_with(name, ids[i])) {
            return i;
        }
    }
    return -1;
}

}  // anonymous namespace

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const For *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(For): " << op->name << "\n";

    if (is_gpu_var(op->name)) {
        internal_assert((op->for_type == ForType::GPUBlock) ||
                        (op->for_type == ForType::GPUThread))
            << "kernel loops must be either gpu block or gpu thread\n";
        // This should always be true at this point in codegen
        internal_assert(is_const_zero(op->min));

        // Save & validate the workgroup size
        int idx = thread_loop_workgroup_index(op->name);
        if (idx >= 0) {
            const IntImm *wsize = op->extent.as<IntImm>();
            user_assert(wsize != nullptr) << "Vulkan requires statically-known workgroup size.\n";
            uint32_t new_wsize = wsize->value;
            user_assert(workgroup_size[idx] == 0 || workgroup_size[idx] == new_wsize) << "Vulkan requires all kernels have the same workgroup size, but two different ones "
                                                                                         "were encountered "
                                                                                      << workgroup_size[idx] << " and " << new_wsize << " in dimension " << idx << "\n";
            workgroup_size[idx] = new_wsize;
        }

        auto intrinsic = simt_intrinsic(op->name);

        // Intrinsics are inserted when adding the kernel
        internal_assert(symbol_table.contains(intrinsic.first));
        SpvId intrinsic_id = symbol_table.get(intrinsic.first).first;

        // extract and cast to int (which is what's expected by Halide's for loops)
        SpvId unsigned_type_id = builder.declare_type(UInt(32));
        SpvId unsigned_gpu_var_id = builder.reserve_id(SpvResultId);
        SpvId signed_type_id = builder.declare_type(Int(32));
        SpvId signed_gpu_var_id = builder.reserve_id(SpvResultId);
        SpvFactory::Indices indices = {intrinsic.second};
        builder.append(SpvFactory::composite_extract(unsigned_type_id, unsigned_gpu_var_id, intrinsic_id, indices));
        builder.append(SpvFactory::bitcast(signed_type_id, signed_gpu_var_id, unsigned_gpu_var_id));
        {
            ScopedSymbolBinding binding(symbol_table, op->name, {signed_gpu_var_id, SpvStorageClassUniform});
            op->body.accept(this);
        }

    } else {

        internal_assert(op->for_type == ForType::Serial) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit unhandled For type: " << op->for_type << "\n";

        // TODO: Loop vars are alway int32_t right?
        SpvId index_type_id = builder.declare_type(Int(32));
        SpvId index_var_type_id = builder.declare_pointer_type(index_type_id, SpvStorageClassFunction);

        op->min.accept(this);
        SpvId min_id = builder.current_id();
        op->extent.accept(this);
        SpvId extent_id = builder.current_id();

        // Compute max.
        SpvId max_id = builder.reserve_id(SpvResultId);
        builder.append(SpvFactory::integer_add(index_type_id, max_id, min_id, extent_id));

        // Declare loop var
        SpvId loop_var_id = builder.declare_variable(unique_name("_loop_idx"), index_var_type_id, SpvStorageClassFunction, min_id);

        SpvId header_block_id = builder.reserve_id(SpvBlockId);
        SpvId top_block_id = builder.reserve_id(SpvBlockId);
        SpvId body_block_id = builder.reserve_id(SpvBlockId);
        SpvId continue_block_id = builder.reserve_id(SpvBlockId);
        SpvId merge_block_id = builder.reserve_id(SpvBlockId);

        SpvBlock header_block = builder.create_block(header_block_id);
        builder.enter_block(header_block);
        {
            builder.append(SpvFactory::loop_merge(merge_block_id, continue_block_id, SpvLoopControlMaskNone));
            builder.append(SpvFactory::branch(top_block_id));
        }
        builder.leave_block();

        SpvId current_index_id = builder.reserve_id(SpvResultId);
        SpvBlock top_block = builder.create_block(top_block_id);
        builder.enter_block(top_block);
        {
            SpvId loop_test_type_id = builder.declare_type(Bool());
            SpvId loop_test_id = builder.reserve_id(SpvResultId);
            builder.append(SpvFactory::load(index_type_id, current_index_id, loop_var_id));
            builder.append(SpvFactory::less_than_equal(loop_test_type_id, loop_test_id, current_index_id, max_id, true));
            builder.append(SpvFactory::conditional_branch(loop_test_id, body_block_id, merge_block_id));
        }
        builder.leave_block();

        SpvBlock body_block = builder.create_block(body_block_id);
        builder.enter_block(body_block);
        {
            ScopedSymbolBinding binding(symbol_table, op->name, {current_index_id, SpvStorageClassFunction});
            op->body.accept(this);
            builder.append(SpvFactory::branch(continue_block_id));
        }
        builder.leave_block();

        SpvBlock continue_block = builder.create_block(continue_block_id);
        builder.enter_block(continue_block);
        {
            // Update loop variable
            int32_t one = 1;
            SpvId next_index_id = builder.reserve_id(SpvResultId);
            SpvId constant_one_id = builder.declare_constant(Int(32), &one);
            builder.append(SpvFactory::integer_add(index_type_id, next_index_id, current_index_id, constant_one_id));
            builder.append(SpvFactory::store(loop_var_id, next_index_id));
            builder.append(SpvFactory::branch(header_block_id));
        }
        builder.leave_block();

        SpvBlock merge_block = builder.create_block(merge_block_id);
        builder.enter_block(merge_block);
    }
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Ramp *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Ramp): "
             << "base=" << op->base << " "
             << "stride=" << op->stride << " "
             << "lanes=" << (uint32_t)op->lanes << "\n";

    // TODO: Is there a way to do this that doesn't require duplicating lane values?
    SpvId base_type_id = builder.declare_type(op->base.type());
    SpvId type_id = builder.declare_type(op->type);
    op->base.accept(this);
    SpvId base_id = builder.current_id();
    op->stride.accept(this);
    SpvId stride_id = builder.current_id();

    // Generate adds to make the elements of the ramp.
    SpvId prev_id = base_id;
    SpvFactory::Components constituents = {base_id};
    for (int i = 1; i < op->lanes; i++) {
        SpvId this_id = builder.reserve_id(SpvResultId);
        if (op->base.type().is_float()) {
            builder.append(SpvFactory::float_add(base_type_id, this_id, prev_id, stride_id));
        } else {
            builder.append(SpvFactory::integer_add(base_type_id, this_id, prev_id, stride_id));
        }
        constituents.push_back(this_id);
        prev_id = this_id;
    }

    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::composite_construct(type_id, result_id, constituents));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Broadcast *op) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(Broadcast): "
             << "type=" << op->type << " "
             << "value=" << op->value << "\n";

    // TODO: Is there a way to do this that doesn't require duplicating lane values?
    SpvId type_id = builder.declare_type(op->type);
    op->value.accept(this);
    SpvId value_id = builder.current_id();
    SpvId result_id = builder.reserve_id(SpvResultId);

    SpvFactory::Components constituents;
    constituents.insert(constituents.end(), op->lanes, value_id);
    builder.append(SpvFactory::composite_construct(type_id, result_id, constituents));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Provide *) {
    internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Provide *): Provide encountered during codegen\n";
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Allocate *op) {

    SpvId base_type_id = SpvInvalidId;
    SpvId variable_id = SpvInvalidId;
    SpvStorageClass storage_class = SpvStorageClassGeneric;
    if (op->memory_type == MemoryType::GPUShared) {
        // Allocation of shared memory must be declared at global scope
        internal_assert(op->extents.size() == 1 && is_const(op->extents[0]));
        int32_t size = op->constant_allocation_size();
        base_type_id = builder.declare_type(op->type, size);
        storage_class = SpvStorageClassWorkgroup;  // shared across workgroup
        debug(2) << "Vulkan: Allocate " << op->name << "[" << (uint32_t)size << "] in shared memory on device in global scope\n";
        SpvId ptr_type_id = builder.declare_pointer_type(base_type_id, storage_class);
        variable_id = builder.declare_global_variable(op->name, ptr_type_id, storage_class);

    } else {

        // Allocation is not a shared memory allocation, just make a local declaration.
        debug(2) << "Vulkan: Allocate " << op->name << " on device in function scope\n";
        int32_t size = op->constant_allocation_size();

        // It must have a constant size.
        user_assert(size > 0)
            << "Allocation " << op->name << " has a dynamic size. "
            << "Only fixed-size allocations are supported on the gpu. "
            << "Try storing into shared memory instead.";

        base_type_id = builder.declare_type(op->type, size);
        storage_class = SpvStorageClassFunction;  // function scope
        SpvId ptr_type_id = builder.declare_pointer_type(base_type_id, storage_class);
        variable_id = builder.declare_variable(op->name, ptr_type_id, storage_class);
    }

    debug(3) << "Vulkan: Pushing allocation called " << op->name << " onto the symbol table\n";
    symbol_table.push(op->name, {variable_id, storage_class});
    op->body.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Free *op) {
    debug(3) << "Vulkan: Popping allocation called " << op->name << " off the symbol table\n";
    internal_assert(symbol_table.contains(op->name));
    symbol_table.pop(op->name);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Realize *) {
    internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Realize *): Realize encountered during codegen\n";
}

template<typename StmtOrExpr>
SpvFactory::BlockVariables
CodeGen_Vulkan_Dev::SPIRV_Emitter::emit_if_then_else(const Expr &condition,
                                                     StmtOrExpr then_case, StmtOrExpr else_case) {
    condition.accept(this);
    SpvId cond_id = builder.current_id();
    SpvId merge_block_id = builder.reserve_id(SpvBlockId);
    SpvId if_block_id = builder.reserve_id(SpvBlockId);
    SpvId then_block_id = builder.reserve_id(SpvBlockId);
    SpvId else_block_id = else_case.defined() ? builder.reserve_id(SpvBlockId) : merge_block_id;

    SpvFactory::BlockVariables block_vars;

    // If Conditional
    SpvBlock if_block = builder.create_block(if_block_id);
    builder.enter_block(if_block);
    {
        debug(2) << "Vulkan: If (" << condition << " )\n";

        builder.append(SpvFactory::selection_merge(merge_block_id, SpvSelectionControlMaskNone));
        builder.append(SpvFactory::conditional_branch(cond_id, then_block_id, else_block_id));
    }
    builder.leave_block();

    // Then block
    SpvBlock then_block = builder.create_block(then_block_id);
    builder.enter_block(then_block);
    {
        then_case.accept(this);
        debug(2) << "Vulkan: Then {" << then_case << " }\n";

        SpvId then_id = builder.current_id();
        builder.append(SpvFactory::branch(merge_block_id));
        block_vars.push_back({then_id, then_block_id});
    }
    builder.leave_block();

    // Else block (optional)
    if (else_case.defined()) {
        SpvBlock else_block = builder.create_block(else_block_id);
        builder.enter_block(else_block);
        {
            else_case.accept(this);
            debug(2) << "Vulkan: Else { " << else_case << " }\n";
            SpvId else_id = builder.current_id();
            builder.append(SpvFactory::branch(merge_block_id));
            block_vars.push_back({else_id, else_block_id});
        }
        builder.leave_block();
    }

    // Merge block
    SpvBlock merge_block = builder.create_block(merge_block_id);
    builder.enter_block(merge_block);
    return block_vars;
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const IfThenElse *op) {
    emit_if_then_else(op->condition, op->then_case, op->else_case);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Evaluate *op) {
    op->value.accept(this);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Shuffle *op) {
    internal_assert(op->vectors.size() == 2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Shuffle *op): SPIR-V codegen currently only supports shuffles of vector pairs.\n";
    SpvId type_id = builder.declare_type(op->type);
    op->vectors[0].accept(this);
    SpvId vector0_id = builder.current_id();
    op->vectors[1].accept(this);
    SpvId vector1_id = builder.current_id();

    SpvFactory::Indices indices;
    indices.insert(indices.end(), op->indices.begin(), op->indices.end());

    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::vector_shuffle(type_id, result_id, vector0_id, vector1_id, indices));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const VectorReduce *) {
    internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const VectorReduce *): VectorReduce not implemented for codegen\n";
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Prefetch *) {
    internal_error << "CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Prefetch *): Prefetch not implemented for codegen\n";
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Fork *) {
    internal_error << "void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Fork *): Fork not implemented for codegen";
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Acquire *) {
    internal_error << "void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Acquire *): Acquire not implemented for codegen";
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Atomic *) {
    internal_error << "void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit(const Atomic *): Atomic not implemented for codegen";
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit_unaryop(Type t, const Expr &a, SpvOp op_code) {
    SpvId type_id = builder.declare_type(t);
    a.accept(this);
    SpvId src_a_id = builder.current_id();

    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::unary_op(op_code, type_id, result_id, src_a_id));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit_binop(Type t, const Expr &a, const Expr &b, SpvOp op_code) {
    SpvId type_id = builder.declare_type(t);
    a.accept(this);
    SpvId src_a_id = builder.current_id();
    b.accept(this);
    SpvId src_b_id = builder.current_id();

    SpvId result_id = builder.reserve_id(SpvResultId);
    builder.append(SpvFactory::binary_op(op_code, type_id, result_id, src_a_id, src_b_id));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit_glsl_unaryop(Type type, const Expr &a, SpvId glsl_op_code) {
    uint32_t type_id = builder.declare_type(type);
    a.accept(this);
    SpvId src_a_id = builder.current_id();

    SpvId inst_set_id = builder.import_glsl_intrinsics();
    SpvId result_id = builder.reserve_id(SpvResultId);
    SpvFactory::Operands operands = {src_a_id};
    builder.append(SpvFactory::extended(inst_set_id, glsl_op_code, type_id, result_id, operands));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::visit_glsl_binop(Type type, const Expr &a, const Expr &b, SpvId glsl_op_code) {
    SpvId type_id = builder.declare_type(type);
    a.accept(this);
    SpvId src_a_id = builder.current_id();
    b.accept(this);
    SpvId src_b_id = builder.current_id();

    SpvId inst_set_id = builder.import_glsl_intrinsics();
    SpvId result_id = builder.reserve_id(SpvResultId);
    SpvFactory::Operands operands = {src_a_id, src_b_id};
    builder.append(SpvFactory::extended(inst_set_id, glsl_op_code, type_id, result_id, operands));
    builder.update_id(result_id);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::init_module() {

    builder.reset();

    // NOTE: Source language is irrelevant. We encode the binary directly
    builder.set_source_language(SpvSourceLanguageUnknown);

    // TODO: Should we autodetect and/or force 32bit or 64bit?
    builder.set_addressing_model(SpvAddressingModelLogical);

    // TODO: Is there a better memory model to use?
    builder.set_memory_model(SpvMemoryModelGLSL450);

    // NOTE: Execution model for Vulkan must be GLCompute which requires Shader support
    builder.require_capability(SpvCapabilityShader);

    // NOTE: Extensions are handled in finalize
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::encode_header(SpvBinary &spirv_header) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::encode_header\n";

    // Encode a sidecar for the module that lists the descriptor sets
    // corresponding to each entry point contained in the module
    //
    // [0] Header word count (total length of header)
    // [1] Number of descriptor sets
    // ... For each descriptor set ...
    // ... [0] Number of uniform buffers for this descriptor set
    // ... [1] Number of storage buffers for this descriptor set
    // ... [2] Length of entry point name (padded to nearest word size)
    // ... [X] Entry point string data
    //

    // NOTE: The Vulkan runtime consumes this header prior to compiling.
    //
    // Both vk_decode_entry_point_data() and vk_compile_shader_module() will
    // need to be updated if the header encoding ever changes!
    //
    uint32_t index = 0;
    spirv_header.push_back(descriptor_set_table.size());
    for (const DescriptorSet &ds : descriptor_set_table) {
        std::vector<char> padded_name;
        uint32_t padded_word_count = (ds.entry_point_name.length() + 3) / 4;
        uint32_t padded_str_length = padded_word_count * 4;
        padded_name.reserve(padded_str_length);
        padded_name.insert(padded_name.begin(), ds.entry_point_name.c_str(), (ds.entry_point_name.c_str() + ds.entry_point_name.length()));
        uint32_t padding = (padded_str_length - ds.entry_point_name.length());
        for (uint32_t i = 0; i < padding; ++i) {
            padded_name.push_back('\0');
        }

        debug(2) << "    [" << index << "] "
                 << "uniform_buffer_count=" << ds.uniform_buffer_count << " "
                 << "storage_buffer_count=" << ds.storage_buffer_count << " "
                 << "entry_point_name_length=" << padded_str_length << " "
                 << "entry_point_name_size=" << padded_name.size() << " "
                 << "entry_point_name: " << (const char *)padded_name.data() << "\n";

        spirv_header.push_back(ds.uniform_buffer_count);
        spirv_header.push_back(ds.storage_buffer_count);
        spirv_header.push_back(padded_str_length);
        internal_assert(padded_name.size() == padded_str_length);
        spirv_header.insert(spirv_header.end(), (const uint32_t *)padded_name.data(), (const uint32_t *)(padded_name.data() + padded_name.size()));
        ++index;
    }
    uint32_t header_word_count = spirv_header.size();
    spirv_header.insert(spirv_header.begin(), header_word_count + 1);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::reset_workgroup_size() {
    workgroup_size[0] = 0;
    workgroup_size[1] = 0;
    workgroup_size[2] = 0;
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::declare_workgroup_size(SpvId kernel_func_id) {
    workgroup_size[0] = std::max(workgroup_size[0], (uint32_t)1);
    workgroup_size[1] = std::max(workgroup_size[1], (uint32_t)1);
    workgroup_size[2] = std::max(workgroup_size[2], (uint32_t)1);

    // Add workgroup size to execution mode
    SpvInstruction exec_mode_inst = SpvFactory::exec_mode_local_size(kernel_func_id, workgroup_size[0], workgroup_size[1], workgroup_size[2]);
    builder.current_module().add_execution_mode(exec_mode_inst);
}

namespace {

// Locate all the unique GPU variables used as SIMT intrinsics
class FindIntrinsicsUsed : public IRVisitor {
    using IRVisitor::visit;
    void visit(const For *op) override {
        if (CodeGen_GPU_Dev::is_gpu_var(op->name)) {
            auto intrinsic = simt_intrinsic(op->name);
            intrinsics_used.insert(intrinsic.first);
        }
        op->body.accept(this);
    }
    void visit(const Variable *op) override {
        if (CodeGen_GPU_Dev::is_gpu_var(op->name)) {
            auto intrinsic = simt_intrinsic(op->name);
            intrinsics_used.insert(intrinsic.first);
        }
    }

public:
    std::unordered_set<std::string> intrinsics_used;
    FindIntrinsicsUsed() = default;
};

// Map the SPIR-V builtin intrinsic name to its corresponding enum value
SpvBuiltIn map_simt_builtin(const std::string &intrinsic_name) {
    if (starts_with(intrinsic_name, "Workgroup")) {
        return SpvBuiltInWorkgroupId;
    } else if (starts_with(intrinsic_name, "Local")) {
        return SpvBuiltInLocalInvocationId;
    }
    internal_error << "map_simt_builtin called on bad variable name: " << intrinsic_name << "\n";
    return SpvBuiltInMax;
}

}  // namespace

void CodeGen_Vulkan_Dev::SPIRV_Emitter::declare_entry_point(const Stmt &s, SpvId kernel_func_id) {

    // Locate all simt intrinsics
    FindIntrinsicsUsed find_intrinsics;
    s.accept(&find_intrinsics);

    SpvFactory::Variables entry_point_variables;
    for (const std::string &intrinsic_name : find_intrinsics.intrinsics_used) {

        // The builtins are pointers to vec3
        SpvId intrinsic_type_id = builder.declare_type(Type(Type::UInt, 32, 3));
        SpvId intrinsic_ptr_type_id = builder.declare_pointer_type(intrinsic_type_id, SpvStorageClassInput);
        SpvId intrinsic_id = builder.declare_global_variable(intrinsic_name, intrinsic_ptr_type_id, SpvStorageClassInput);
        SpvId intrinsic_loaded_id = builder.reserve_id();
        builder.append(SpvFactory::load(intrinsic_type_id, intrinsic_loaded_id, intrinsic_id));
        symbol_table.push(intrinsic_name, {intrinsic_loaded_id, SpvStorageClassInput});

        // Annotate that this is the specific builtin
        SpvBuiltIn built_in_kind = map_simt_builtin(intrinsic_name);
        SpvBuilder::Literals annotation_literals = {(uint32_t)built_in_kind};
        builder.add_annotation(intrinsic_id, SpvDecorationBuiltIn, annotation_literals);

        // Add the builtin to the interface
        entry_point_variables.push_back(intrinsic_id);
    }

    // Add the entry point with the appropriate execution model
    // NOTE: exec_model must be GLCompute to work with Vulkan ... Kernel is only supported in OpenCL
    builder.add_entry_point(kernel_func_id, SpvExecutionModelGLCompute, entry_point_variables);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::declare_device_args(uint32_t entry_point_index,
                                                            const std::string &entry_point_name,
                                                            const std::vector<DeviceArgument> &args) {

    // Keep track of the descriptor set needed to bind this kernel's inputs / outputs
    DescriptorSet descriptor_set;
    descriptor_set.entry_point_name = entry_point_name;

    // Add required extension support for storage types
    for (const auto &arg : args) {
        if (arg.is_buffer) {
            if (arg.type.is_int_or_uint()) {
                if (arg.type.bits() == 8) {
                    builder.require_extension("SPV_KHR_8bit_storage");
                } else if (arg.type.bits() == 16) {
                    builder.require_extension("SPV_KHR_16bit_storage");
                }
            }
        }
    }

    // GLSL-style: each input buffer is a runtime array in a buffer struct
    // All other params get passed in as a single uniform block
    // First, need to count scalar parameters to construct the uniform struct
    SpvBuilder::StructMemberTypes param_struct_members;
    for (const auto &arg : args) {
        if (!arg.is_buffer) {
            SpvId arg_type_id = builder.declare_type(arg.type);
            param_struct_members.push_back(arg_type_id);
        }
    }

    // Add a binding for a uniform buffer packed with all scalar args
    uint32_t binding_counter = 0;
    if (!param_struct_members.empty()) {
        const std::string struct_name = std::string("_struct") + entry_point_name + std::string("_args");
        SpvId param_struct_type_id = builder.declare_struct(struct_name, param_struct_members);

        // Add a decoration describing the offset for each parameter struct member
        uint32_t param_member_index = 0;
        uint32_t param_member_offset = 0;
        for (const auto &arg : args) {
            if (!arg.is_buffer) {
                SpvBuilder::Literals param_offset_literals = {param_member_offset};
                builder.add_struct_annotation(param_struct_type_id, param_member_index, SpvDecorationOffset, param_offset_literals);
                param_member_offset += arg.type.bytes();
                param_member_index++;
            }
        }

        // Add a Block decoration for the parameter pack itself
        builder.add_annotation(param_struct_type_id, SpvDecorationBlock);

        // Add a variable for the parameter pack
        const std::string param_pack_var_name = std::string("_var") + entry_point_name + std::string("_args");
        SpvId param_pack_ptr_type_id = builder.declare_pointer_type(param_struct_type_id, SpvStorageClassUniform);
        SpvId param_pack_var_id = builder.declare_global_variable(param_pack_var_name, param_pack_ptr_type_id, SpvStorageClassUniform);

        // We always pass in the parameter pack as the first binding
        SpvBuilder::Literals binding_index = {0};
        SpvBuilder::Literals dset_index = {entry_point_index};
        builder.add_annotation(param_pack_var_id, SpvDecorationDescriptorSet, dset_index);
        builder.add_annotation(param_pack_var_id, SpvDecorationBinding, binding_index);
        descriptor_set.uniform_buffer_count++;
        binding_counter++;

        // Declare all the args with appropriate offsets into the parameter struct
        uint32_t scalar_index = 0;
        for (const auto &arg : args) {
            if (!arg.is_buffer) {

                SpvId arg_type_id = builder.declare_type(arg.type);
                SpvId access_index_id = builder.declare_constant(UInt(32), &scalar_index);
                SpvId pointer_type_id = builder.declare_pointer_type(arg_type_id, SpvStorageClassUniform);
                SpvFactory::Indices access_indices = {access_index_id};
                SpvId access_chain_id = builder.declare_access_chain(pointer_type_id, param_pack_var_id, access_indices);
                scalar_index++;

                SpvId param_id = builder.reserve_id(SpvResultId);
                builder.append(SpvFactory::load(arg_type_id, param_id, access_chain_id));
                symbol_table.push(arg.name, {param_id, SpvStorageClassUniform});
            }
        }
    }

    // Add bindings for all device buffers as uniform buffers
    for (const auto &arg : args) {
        if (arg.is_buffer) {

            // Declare the runtime array (which maps directly to the Halide device buffer)
            SpvId element_type_id = builder.declare_type(arg.type);
            SpvId runtime_arr_type_id = builder.add_runtime_array(element_type_id);

            // Annotate the array with its stride
            SpvBuilder::Literals array_stride = {(uint32_t)(arg.type.bytes())};
            builder.add_annotation(runtime_arr_type_id, SpvDecorationArrayStride, array_stride);

            // Wrap the runtime array in a struct (required with SPIR-V buffer block semantics)
            SpvBuilder::StructMemberTypes struct_member_types = {runtime_arr_type_id};
            const std::string struct_name = std::string("_struct") + entry_point_name + std::string("_b") + std::to_string(binding_counter);
            SpvId struct_type_id = builder.declare_struct(struct_name, struct_member_types);

            // Declare a pointer to the struct as a global variable
            SpvStorageClass storage_class = SpvStorageClassUniform;
            SpvId ptr_struct_type_id = builder.declare_pointer_type(struct_type_id, storage_class);
            SpvId param_id = builder.declare_global_variable(arg.name, ptr_struct_type_id, storage_class);

            // Annotate the struct to indicate it's passed in a GLSL-style buffer block
            builder.add_annotation(struct_type_id, SpvDecorationBufferBlock);

            // Annotate the offset for the array
            SpvBuilder::Literals zero_literal = {uint32_t(0)};
            builder.add_struct_annotation(struct_type_id, 0, SpvDecorationOffset, zero_literal);

            // Set descriptor set and binding indices
            SpvBuilder::Literals dset_index = {entry_point_index};
            SpvBuilder::Literals binding_index = {uint32_t(binding_counter++)};
            builder.add_annotation(param_id, SpvDecorationDescriptorSet, dset_index);
            builder.add_annotation(param_id, SpvDecorationBinding, binding_index);
            symbol_table.push(arg.name, {param_id, storage_class});
            descriptor_set.storage_buffer_count++;
        }
    }

    // Save the descriptor set (so we can output the binding information as a header to the code module)
    descriptor_set_table.push_back(descriptor_set);
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::compile(std::vector<char> &module) {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::compile\n";

    // First encode the descriptor set bindings for each entry point
    // as a sidecar which we will add as a preamble header to the actual
    // SPIR-V binary so the runtime can know which descriptor set to use
    // for each entry point
    SpvBinary spirv_header;
    encode_header(spirv_header);

    // Finalize and encode the SPIR-V IR into a compliant binary
    SpvBinary spirv_binary;
    builder.finalize();
    builder.encode(spirv_binary);

    size_t header_bytes = spirv_header.size() * sizeof(uint32_t);
    size_t binary_bytes = spirv_binary.size() * sizeof(uint32_t);

    debug(2) << "    encoding module ("
             << "header_size: " << (uint32_t)(header_bytes) << ", "
             << "binary_size: " << (uint32_t)(binary_bytes) << ")\n";

    // Combine the header and binary into the module
    module.reserve(header_bytes + binary_bytes);
    module.insert(module.end(), (const char *)spirv_header.data(), (const char *)(spirv_header.data() + spirv_header.size()));
    module.insert(module.end(), (const char *)spirv_binary.data(), (const char *)(spirv_binary.data() + spirv_binary.size()));
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::add_kernel(const Stmt &s,
                                                   const std::string &name,
                                                   const std::vector<DeviceArgument> &args) {
    debug(2) << "Adding Vulkan kernel " << name << "\n";

    // Add function definition
    // TODO: can we use one of the function control annotations?

    // We'll discover the workgroup size as we traverse the kernel
    reset_workgroup_size();

    // Declare the kernel function
    SpvId void_type_id = builder.declare_void_type();
    SpvId kernel_func_id = builder.add_function(name, void_type_id);
    SpvFunction kernel_func = builder.lookup_function(kernel_func_id);
    uint32_t entry_point_index = builder.current_module().entry_point_count();
    builder.enter_function(kernel_func);

    // Declare the entry point and input intrinsics for the kernel func
    declare_entry_point(s, kernel_func_id);

    // Declare all parameters -- scalar args and device buffers
    declare_device_args(entry_point_index, name, args);
    /*
        // TODO: only add the SIMT intrinsics used
        SpvFactory::Variables entry_point_variables;
        auto intrinsics = {"WorkgroupId", "LocalInvocationId"};
        for (const std::string &intrinsic_name : intrinsics) {

            // The builtins are pointers to vec3
            SpvId intrinsic_type_id = builder.declare_type(Type(Type::UInt, 32, 3));
            SpvId intrinsic_ptr_type_id = builder.declare_pointer_type(intrinsic_type_id, SpvStorageClassInput);
            SpvId intrinsic_id = builder.declare_global_variable(intrinsic_name, intrinsic_ptr_type_id, SpvStorageClassInput);
            SpvId intrinsic_loaded_id = builder.reserve_id();
            builder.append(SpvFactory::load(intrinsic_type_id, intrinsic_loaded_id, intrinsic_id));
            symbol_table.push(intrinsic_name, {intrinsic_loaded_id, SpvStorageClassInput});

            // Annotate that this is the specific builtin
            SpvBuiltIn built_in_kind = starts_with(intrinsic_name, "Workgroup") ? SpvBuiltInWorkgroupId : SpvBuiltInLocalInvocationId;
            SpvBuilder::Literals annotation_literals = {(uint32_t)built_in_kind};
            builder.add_annotation(intrinsic_id, SpvDecorationBuiltIn, annotation_literals);

            // Add the builtin to the interface
            entry_point_variables.push_back(intrinsic_id);
        }

        // Add the entry point with the appropriate execution model
        // NOTE: exec_model must be GLCompute to work with Vulkan ... Kernel is only supported in OpenCL
        uint32_t current_entry_point = builder.current_module().entry_point_count();
        builder.add_entry_point(kernel_func_id, SpvExecutionModelGLCompute, entry_point_variables);
    */

    /*
        // GLSL-style: each input buffer is a runtime array in a buffer struct
        // All other params get passed in as a single uniform block
        // First, need to count scalar parameters to construct the uniform struct
        SpvBuilder::StructMemberTypes param_struct_members;
        for (const auto &arg : args) {
            if (!arg.is_buffer) {
                SpvId arg_type_id = builder.declare_type(arg.type);
                param_struct_members.push_back(arg_type_id);
            }
        }

        // Add a binding for a uniform buffer packed with all scalar args
        uint32_t binding_counter = 0;
        if (!param_struct_members.empty()) {
            const std::string struct_name = std::string("_struct") + name + std::string("_args");
            SpvId param_struct_type_id = builder.declare_struct(struct_name, param_struct_members);

            // Add a decoration describing the offset for each parameter struct member
            uint32_t param_member_index = 0;
            uint32_t param_member_offset = 0;
            for (const auto &arg : args) {
                if (!arg.is_buffer) {
                    SpvBuilder::Literals param_offset_literals = {param_member_offset};
                    builder.add_struct_annotation(param_struct_type_id, param_member_index, SpvDecorationOffset, param_offset_literals);
                    param_member_offset += arg.type.bytes();
                    param_member_index++;
                }
            }

            // Add a Block decoration for the parameter pack itself
            builder.add_annotation(param_struct_type_id, SpvDecorationBlock);

            // Add a variable for the parameter pack
            const std::string param_pack_var_name = std::string("_var") + name + std::string("_args");
            SpvId param_pack_ptr_type_id = builder.declare_pointer_type(param_struct_type_id, SpvStorageClassUniform);
            SpvId param_pack_var_id = builder.declare_global_variable(param_pack_var_name, param_pack_ptr_type_id, SpvStorageClassUniform);

            // We always pass in the parameter pack as the first binding
            SpvBuilder::Literals binding_index = {0};
            SpvBuilder::Literals dset_index = {current_entry_point};
            builder.add_annotation(param_pack_var_id, SpvDecorationDescriptorSet, dset_index);
            builder.add_annotation(param_pack_var_id, SpvDecorationBinding, binding_index);
            descriptor_set.uniform_buffer_count++;
            binding_counter++;

            // Declare all the args with appropriate offsets into the parameter struct
            uint32_t scalar_index = 0;
            for (const auto &arg : args) {
                if (!arg.is_buffer) {

                    SpvId arg_type_id = builder.declare_type(arg.type);
                    SpvId access_index_id = builder.declare_constant(UInt(32), &scalar_index);
                    SpvId pointer_type_id = builder.declare_pointer_type(arg_type_id, SpvStorageClassUniform);
                    SpvFactory::Indices access_indices = {access_index_id};
                    SpvId access_chain_id = builder.declare_access_chain(pointer_type_id, param_pack_var_id, access_indices);
                    scalar_index++;

                    SpvId param_id = builder.reserve_id(SpvResultId);
                    builder.append(SpvFactory::load(arg_type_id, param_id, access_chain_id));
                    symbol_table.push(arg.name, {param_id, SpvStorageClassUniform});
                }
            }
        }

        // Add bindings for all device buffers
        for (const auto &arg : args) {
            if (arg.is_buffer) {

                // Add required extension support for storage types
                if (arg.type.is_int_or_uint()) {
                    if (arg.type.bits() == 8) {
                        builder.require_extension("SPV_KHR_8bit_storage");
                    } else if (arg.type.bits() == 16) {
                        builder.require_extension("SPV_KHR_16bit_storage");
                    }
                }

                SpvId element_type_id = builder.declare_type(arg.type);
                SpvId runtime_arr_type_id = builder.add_runtime_array(element_type_id);
                SpvBuilder::StructMemberTypes struct_member_types = {runtime_arr_type_id};
                const std::string struct_name = std::string("_struct") + name + std::string("_b") + std::to_string(binding_counter);
                SpvId struct_type_id = builder.declare_struct(struct_name, struct_member_types);
                SpvId ptr_struct_type_id = builder.declare_pointer_type(struct_type_id, SpvStorageClassUniform);
                SpvId param_id = builder.declare_global_variable(arg.name, ptr_struct_type_id, SpvStorageClassUniform);

                // Annotate the struct to indicate it's passed in a GLSL-style buffer block
                builder.add_annotation(struct_type_id, SpvDecorationBufferBlock);

                // Annotate the array with its stride
                SpvBuilder::Literals array_stride = {(uint32_t)(arg.type.bytes())};
                builder.add_annotation(runtime_arr_type_id, SpvDecorationArrayStride, array_stride);

                // Annotate the offset for the array
                SpvBuilder::Literals zero_literal = {uint32_t(0)};
                builder.add_struct_annotation(struct_type_id, 0, SpvDecorationOffset, zero_literal);

                // Set DescriptorSet and Binding
                SpvBuilder::Literals dset_index = {current_entry_point};
                SpvBuilder::Literals binding_index = {uint32_t(binding_counter++)};
                builder.add_annotation(param_id, SpvDecorationDescriptorSet, dset_index);
                builder.add_annotation(param_id, SpvDecorationBinding, binding_index);
                symbol_table.push(arg.name, {param_id, SpvStorageClassUniform});
                descriptor_set.storage_buffer_count++;
            }
        }
        descriptor_set_table.push_back(descriptor_set);
    */
    s.accept(this);

    // Insert return statement end delimiter
    kernel_func.tail_block().add_instruction(SpvFactory::return_stmt());

    // Declare the workgroup size now that we've traversed the kernel
    declare_workgroup_size(kernel_func_id);

    // Pop scope
    for (const auto &arg : args) {
        symbol_table.pop(arg.name);
    }
    builder.leave_block();
    builder.leave_function();
}

void CodeGen_Vulkan_Dev::SPIRV_Emitter::dump() const {
    debug(2) << "CodeGen_Vulkan_Dev::SPIRV_Emitter::dump()\n";
    std::cerr << builder.current_module();
}

CodeGen_Vulkan_Dev::CodeGen_Vulkan_Dev(Target t) {
}

void CodeGen_Vulkan_Dev::init_module() {
    debug(2) << "CodeGen_Vulkan_Dev::init_module\n";
    emitter.init_module();
}

void CodeGen_Vulkan_Dev::add_kernel(Stmt stmt,
                                    const std::string &name,
                                    const std::vector<DeviceArgument> &args) {

    debug(2) << "CodeGen_Vulkan_Dev::add_kernel " << name << "\n";

    // We need to scalarize/de-predicate any loads/stores, since Vulkan does not support predication.
    stmt = scalarize_predicated_loads_stores(stmt);

    debug(2) << "CodeGen_Vulkan_Dev: after removing predication: \n"
             << stmt;

    current_kernel_name = name;
    emitter.add_kernel(stmt, name, args);

    // dump the SPIRV file if requested
    if (getenv("HL_SPIRV_DUMP_FILE")) {
        dump();
    }
}

std::vector<char> CodeGen_Vulkan_Dev::compile_to_src() {
    debug(2) << "CodeGen_Vulkan_Dev::compile_to_src\n";
    std::vector<char> module;
    emitter.compile(module);
    return module;
}

std::string CodeGen_Vulkan_Dev::get_current_kernel_name() {
    return current_kernel_name;
}

std::string CodeGen_Vulkan_Dev::print_gpu_name(const std::string &name) {
    return name;
}

void CodeGen_Vulkan_Dev::dump() {
    std::vector<char> module = compile_to_src();

    // Print the contents of the compiled SPIR-V module
    emitter.dump();

    // Skip the header and only output the SPIR-V binary
    const uint32_t *decode = (const uint32_t *)(module.data());
    uint32_t header_word_count = decode[0];
    size_t header_size = header_word_count * sizeof(uint32_t);
    const uint32_t *binary_ptr = (decode + header_word_count);
    size_t binary_size = (module.size() - header_size);

    const char *filename = getenv("HL_SPIRV_DUMP_FILE") ? getenv("HL_SPIRV_DUMP_FILE") : "out.spv";
    debug(1) << "Vulkan: Dumping SPIRV module to file: '" << filename << "'\n";
    std::ofstream f(filename, std::ios::out | std::ios::binary);
    f.write((const char *)(binary_ptr), binary_size);
    f.close();
}

}  // namespace

std::unique_ptr<CodeGen_GPU_Dev> new_CodeGen_Vulkan_Dev(const Target &target) {
    return std::make_unique<CodeGen_Vulkan_Dev>(target);
}

}  // namespace Internal
}  // namespace Halide

#else  // WITH_SPIRV

namespace Halide {
namespace Internal {

std::unique_ptr<CodeGen_GPU_Dev> new_CodeGen_Vulkan_Dev(const Target &target) {
    return nullptr;
}

}  // namespace Internal
}  // namespace Halide

#endif  // WITH_SPIRV
