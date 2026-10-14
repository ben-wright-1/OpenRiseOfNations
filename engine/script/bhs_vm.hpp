// The BigHugeScript runtime (docs/script-spec.md sections 5-6): script values (ScriptType and subclasses), loaded
// script files (ScriptFile / Script, from compiled images), and the virtual machine with RunTimeEnv's run-to-completion
// protocol (run_script @9c4460, exec @9c3600, VirtualMachine::execute_next @9e0840, call_script / close_frame,
// call_func @9e0550 with ScriptFuncSet::call_func @9d5500).
//
// Values are reference objects as in the original: PUSH pushes the variable's own object, the assign family mutates
// its target in place and every other operator makes a new temporary. Lifetime is handled by shared ownership; the
// original's scopes (TEMP / CONST / VAR) are kept because they decide adoption versus copying (duplicate,
// VirtualMachine::set_value).
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "script/bhs_builtins.hpp"
#include "script/bhs_common.hpp"

namespace ron::bhs {

enum Scope : uint8_t { SC_TEMP = 1, SC_CONST = 2, SC_VAR = 3 };

struct Value;
using VP = std::shared_ptr<Value>;

struct Value {
	enum Cls : uint8_t { Int, Float, String, Array, Object };
	Cls cls = Int;
	uint8_t scope = SC_TEMP;
	int32_t data_type = TY_INT;
	int32_t i = 0;
	float f = 0;
	U16 s;
	std::vector<VP> elems;  // array elements / struct members
	VP blank;               // array growth template (blank_base)
	bool is_array() const { return cls == Array; }
};

VP make_int(int32_t v, uint8_t scope = SC_TEMP);
VP make_float(float v, uint8_t scope = SC_TEMP);
VP make_string(const U16& v, uint8_t scope = SC_TEMP);
VP duplicate(const VP& v);  // ScriptType::duplicate: a temporary is adopted, anything else copied (deep)
int32_t value_int(const Value* v);
float value_float(const Value* v);
U16 value_string(const Value* v);
bool value_is_false(const Value* v);

// A script of a loaded file (Script, 0xcc).
struct RtScript {
	U16 name;
	int32_t offset = 0, return_type = TY_VOID, script_type = 0;
	std::vector<int32_t> params;
	std::vector<uint8_t> refs;
	std::vector<VP> static_vars;          // saved: the static variables
	int32_t trigger_bit_count = 0;        // DynamicBitMask.bits
	std::vector<uint8_t> trigger_bits;    // saved: 1 = enabled
	std::vector<U16> trigger_names, var_names, static_var_names;
	bool trigger_enabled(int32_t n) const;  // Script::is_trigger_enabled (-1 when out of range counts as enabled)
	void set_trigger(int32_t n, bool on);
};

// A loaded script file (ScriptFile, 0xe0).
struct RtFile {
	std::string source_file;  // the compiler's input_string (case-insensitive key)
	std::vector<uint8_t> code;
	std::vector<RtScript> scripts;
	std::vector<VP> const_pool;
	std::vector<U16> linked_file_names;
	std::vector<int32_t> linked_files;  // indices into Runtime::files (-1 unresolved)
	std::vector<int32_t> line_to_op;
};

class Runtime;

// A builtin call: args[i] is argument i (the VM's own objects: writing through a by-reference argument changes the
// caller's variable). Return the result value (a temporary), or null for void builtins.
struct NativeCall {
	Runtime& rt;
	const BuiltinFunc& func;
	std::vector<VP>& args;
	int32_t arg_int(size_t k) const { return k < args.size() ? value_int(args[k].get()) : 0; }
	float arg_float(size_t k) const { return k < args.size() ? value_float(args[k].get()) : 0.f; }
	U16 arg_str(size_t k) const { return k < args.size() ? value_string(args[k].get()) : U16(); }
};
using Native = std::function<VP(NativeCall&)>;

class Runtime {
public:
	explicit Runtime(const BuiltinRegistry& reg = BuiltinRegistry::standard());

	// ---- files (ScriptFile::script_files)
	std::vector<std::unique_ptr<RtFile>> files;
	// ScriptFile::load_script_file (SCRIPT_RELOAD_FULL): replaces a file with the same source path or appends one.
	int load_image(const std::string& source, const Bytes& image, std::string* err = nullptr);
	// ScriptFile::find_script: files from the last loaded backwards, scripts in order, case-insensitive.
	RtScript* find_script(const U16& name, int* file_index = nullptr);

	// ---- running (RunTimeEnv)
	enum Status { kOk = 0, kBreak = 2, kNotFound = 3, kBadArgs = 4, kError = 6 };
	// Runs a script to completion. args[0] is argument 0; each must have exactly the parameter's type.
	int run_script(const U16& name, const std::vector<VP>& args);
	VP return_value;            // RunTimeEnv::return_value of the last successful run
	int32_t ret_int() const { return return_value ? value_int(return_value.get()) : 0; }
	uint32_t bytecodes_executed = 0;
	int err_count = 0;
	void run_time_error(const std::string& msg);  // marks the VM, status ERROR, reports through `log`
	RtScript* current_script();                    // the running frame's script (TriggerUtil), or null
	int current_file();

	// ---- builtins
	const BuiltinRegistry& registry;
	std::vector<Native> natives;  // by global index; empty = not implemented (logged once, returns a default)
	void set_native(const std::string& name, Native fn, int overload = 0);  // overload = k-th registration of name
	std::vector<int> unported_calls;  // per global index: calls to builtins without an implementation
	std::function<void(const std::string&)> log;  // run-time errors and warnings (default: none)

	// Value helpers for natives
	static VP err_return(int32_t type);  // ScriptFuncSet::get_err_return: int -1, float -1.0, string "", void null

private:
	struct Frame {
		RtFile* file = nullptr;
		RtScript* script = nullptr;
		int32_t bip = 0;
		size_t expected = 0;
		uint8_t flags = 0;  // 2 returned, 4 error, 8 void
		std::vector<VP> vars;
	};
	std::deque<Frame> frames_;  // references stay valid when a call pushes a frame
	std::vector<VP> stack_;
	int status_ = 0;  // ScriptRunTimeStates: 0 running, 2 clear, 3 error, 4 success

	void clear();
	int exec();
	void execute_next(Frame& f, int op);
	void call_script(int link, int script);
	void close_frame();
	void call_func(Frame& f, int nargs, int index);
	VP get_value(Frame& f, uint32_t operand);
	void set_value(Frame& f, uint32_t operand, VP v);
	VP pop();
	VP do_operator(const VP& a, int op, const VP& b);
	VP init_simple(const Value* src, int32_t type);
};

}  // namespace ron::bhs
