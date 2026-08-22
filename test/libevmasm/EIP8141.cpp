/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Tests covering the EIP-8141 (Frame Transaction) opcode wiring:
 * APPROVE, TXPARAM, FRAMEDATALOAD, FRAMEDATACOPY, FRAMEPARAM, SIGPARAM, SIGDATACOPY.
 */

#include <libevmasm/AssemblyItem.h>
#include <libevmasm/GasMeter.h>
#include <libevmasm/Instruction.h>
#include <libevmasm/KnownState.h>
#include <libevmasm/SemanticInformation.h>
#include <liblangutil/EVMVersion.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <memory>
#include <vector>

using namespace solidity::langutil;
using namespace solidity::evmasm;

namespace solidity::frontend::test
{

namespace
{
	struct OpcodeSpec
	{
		Instruction instruction;
		std::string name;
		uint8_t opcode;
		int args;
		int ret;
		bool sideEffects;
		Tier tier;
	};

	std::array<OpcodeSpec, 7> const c_eip8141Opcodes = {{
		{Instruction::APPROVE,       "APPROVE",       0xaa, 3, 0, true,  Tier::Zero},
		{Instruction::TXPARAM,       "TXPARAM",       0xb0, 1, 1, false, Tier::Base},
		{Instruction::FRAMEDATALOAD, "FRAMEDATALOAD", 0xb1, 2, 1, false, Tier::VeryLow},
		{Instruction::FRAMEDATACOPY, "FRAMEDATACOPY", 0xb2, 4, 0, true,  Tier::VeryLow},
		{Instruction::FRAMEPARAM,    "FRAMEPARAM",    0xb3, 2, 1, false, Tier::Base},
		{Instruction::SIGPARAM,      "SIGPARAM",      0xb4, 2, 1, false, Tier::Base},
		{Instruction::SIGDATACOPY,   "SIGDATACOPY",   0xb5, 4, 0, true,  Tier::VeryLow},
	}};
	/// Meters @a _instruction on a fresh gas meter after pushing @a _operands, bottom-most operand
	/// first, and returns the total cost including the pushes. The items are kept alive for the
	/// whole metering because KnownState stores pointers to the items it is fed.
	GasMeter::GasConsumption instructionGas(
		std::vector<u256> const& _operands,
		Instruction _instruction,
		EVMVersion _version
	)
	{
		AssemblyItems items;
		for (u256 const& operand: _operands)
			items.emplace_back(operand);
		items.emplace_back(_instruction);

		GasMeter meter(std::make_shared<KnownState>(), _version);
		GasMeter::GasConsumption gas;
		for (AssemblyItem const& item: items)
			gas += meter.estimateMax(item);
		return gas;
	}
}

BOOST_AUTO_TEST_SUITE(EIP8141)

BOOST_AUTO_TEST_CASE(opcode_byte_values_match_spec)
{
	for (auto const& spec: c_eip8141Opcodes)
		BOOST_CHECK_EQUAL(static_cast<uint8_t>(spec.instruction), spec.opcode);
}

BOOST_AUTO_TEST_CASE(instruction_info_round_trip)
{
	for (auto const& spec: c_eip8141Opcodes)
	{
		InstructionInfo const info = instructionInfo(spec.instruction, EVMVersion::osaka().withEIP8141());
		BOOST_CHECK_EQUAL(info.name, spec.name);
		BOOST_CHECK_EQUAL(info.args, spec.args);
		BOOST_CHECK_EQUAL(info.ret, spec.ret);
		BOOST_CHECK_EQUAL(info.sideEffects, spec.sideEffects);
		BOOST_CHECK(info.gasPriceTier == spec.tier);
	}
}

BOOST_AUTO_TEST_CASE(name_to_instruction_lookup)
{
	for (auto const& spec: c_eip8141Opcodes)
	{
		auto it = c_instructions.find(spec.name);
		BOOST_REQUIRE(it != c_instructions.end());
		BOOST_CHECK(it->second == spec.instruction);
	}
}

BOOST_AUTO_TEST_CASE(gated_off_without_flag)
{
	// Without the EIP-8141 flag, no base hardfork must expose the new opcodes — including
	// Osaka under EOF, where the opcode byte values fall in the EOF reserved range.
	for (auto const& spec: c_eip8141Opcodes)
	{
		for (auto const& version: EVMVersion::allVersions())
			BOOST_CHECK_MESSAGE(
				!version.hasOpcode(spec.instruction, std::nullopt),
				spec.name + " unexpectedly enabled on " + version.name()
			);
		// EOF can only be paired with versions >= Osaka per hasOpcode's precondition.
		BOOST_CHECK_MESSAGE(
			!EVMVersion::osaka().hasOpcode(spec.instruction, std::optional<uint8_t>{1}),
			spec.name + " unexpectedly enabled on osaka (EOF)"
		);
	}
}

BOOST_AUTO_TEST_CASE(flag_enables_opcodes_on_any_base_version)
{
	// EIP-8141 is exposed as an orthogonal flag rather than a hardfork, so enabling it on top
	// of any base version must unlock the opcodes regardless of the underlying version.
	std::array<EVMVersion, 4> const baseVersions = {{
		EVMVersion::cancun(),
		EVMVersion::prague(),
		EVMVersion::osaka(),
		EVMVersion::future(),
	}};
	for (auto const& spec: c_eip8141Opcodes)
		for (auto const& base: baseVersions)
		{
			BOOST_CHECK_MESSAGE(
				base.withEIP8141().hasOpcode(spec.instruction, std::nullopt),
				spec.name + " not enabled on " + base.name() + " with EIP-8141 flag"
			);
			// Without the flag, even the latest version must not expose the opcodes.
			BOOST_CHECK_MESSAGE(
				!base.hasOpcode(spec.instruction, std::nullopt),
				spec.name + " unexpectedly enabled on " + base.name() + " without flag"
			);
		}
}

BOOST_AUTO_TEST_CASE(flag_does_not_unlock_unrelated_features)
{
	// The flag is orthogonal: it must not silently bestow features that belong to later hardforks.
	BOOST_CHECK(!EVMVersion::homestead().withEIP8141().hasMcopy());
	BOOST_CHECK(!EVMVersion::homestead().withEIP8141().supportsTransientStorage());
	// Conversely, enabling the flag on Cancun must preserve Cancun-era features.
	BOOST_CHECK(EVMVersion::cancun().withEIP8141().hasMcopy());
	BOOST_CHECK(EVMVersion::cancun().withEIP8141().supportsTransientStorage());
}

BOOST_AUTO_TEST_CASE(version_string_round_trip)
{
	// The flag is rendered as a "+eip8141" suffix on the base hardfork name.
	BOOST_CHECK_EQUAL(EVMVersion::osaka().withEIP8141().name(), "osaka+eip8141");
	BOOST_CHECK_EQUAL(EVMVersion::cancun().withEIP8141().name(), "cancun+eip8141");
	BOOST_CHECK_EQUAL(EVMVersion::osaka().name(), "osaka");

	auto parsed = EVMVersion::fromString("osaka+eip8141");
	BOOST_REQUIRE(parsed.has_value());
	BOOST_CHECK(*parsed == EVMVersion::osaka().withEIP8141());

	auto parsedBase = EVMVersion::fromString("osaka");
	BOOST_REQUIRE(parsedBase.has_value());
	BOOST_CHECK(*parsedBase == EVMVersion::osaka());
	BOOST_CHECK(!parsedBase->hasEIP8141Opcodes());

	// Bare "eip8141" is no longer a recognised version.
	BOOST_CHECK(!EVMVersion::fromString("eip8141").has_value());
}

BOOST_AUTO_TEST_CASE(flag_does_not_appear_in_all_versions)
{
	// allVersions() lists only base hardforks; the flag is an orthogonal axis and must not
	// appear there to avoid combinatorial explosion in test fixtures that iterate versions.
	auto const versions = EVMVersion::allVersions();
	for (auto const& v: versions)
		BOOST_CHECK(!v.hasEIP8141Opcodes());
}

BOOST_AUTO_TEST_CASE(static_gas_costs_match_spec)
{
	// EIP-8141 spec: TXPARAM = 2, FRAMEPARAM = 2, SIGPARAM = 2 (Base tier),
	// FRAMEDATALOAD = 3 (VeryLow).
	BOOST_CHECK_EQUAL(GasMeter::runGas(Instruction::TXPARAM, EVMVersion::osaka().withEIP8141()), 2);
	BOOST_CHECK_EQUAL(GasMeter::runGas(Instruction::FRAMEPARAM, EVMVersion::osaka().withEIP8141()), 2);
	BOOST_CHECK_EQUAL(GasMeter::runGas(Instruction::SIGPARAM, EVMVersion::osaka().withEIP8141()), 2);
	BOOST_CHECK_EQUAL(GasMeter::runGas(Instruction::FRAMEDATALOAD, EVMVersion::osaka().withEIP8141()), 3);
	// The copy instructions' static portion is 3; the dynamic portion (per-word + memory
	// expansion) is covered by copy_instructions_priced_like_calldatacopy below.
	BOOST_CHECK_EQUAL(GasMeter::runGas(Instruction::FRAMEDATACOPY, EVMVersion::osaka().withEIP8141()), 3);
	BOOST_CHECK_EQUAL(GasMeter::runGas(Instruction::SIGDATACOPY, EVMVersion::osaka().withEIP8141()), 3);
}

BOOST_AUTO_TEST_CASE(copy_instructions_priced_like_calldatacopy)
{
	// The spec prices FRAMEDATACOPY and SIGDATACOPY exactly as CALLDATACOPY: the fixed cost of 3,
	// the per-word copy cost and the standard memory expansion cost.
	// Operands bottom-up: the frame or signature index, then length, dataOffset and memOffset on
	// top. CALLDATACOPY reads the same three top-most operands, so it can be metered with the
	// identical sequence and used as the reference.
	std::vector<u256> const operands{u256(3), u256(64), u256(0), u256(128)};
	EVMVersion const version = EVMVersion::osaka().withEIP8141();
	GasMeter::GasConsumption const reference = instructionGas(operands, Instruction::CALLDATACOPY, version);
	BOOST_REQUIRE(!reference.isInfinite);
	for (auto instruction: {Instruction::FRAMEDATACOPY, Instruction::SIGDATACOPY})
	{
		GasMeter::GasConsumption const gas = instructionGas(operands, instruction, version);
		BOOST_CHECK_MESSAGE(!gas.isInfinite, instructionInfo(instruction, version).name + " has unbounded gas cost");
		BOOST_CHECK_EQUAL(gas.value, reference.value);
	}
}

BOOST_AUTO_TEST_CASE(approve_priced_like_return)
{
	// The spec gives APPROVE no base execution-gas cost: it charges only the memory expansion for
	// the return-data region [offset, offset + length), exactly as RETURN does.
	EVMVersion const version = EVMVersion::osaka().withEIP8141();
	BOOST_CHECK_EQUAL(GasMeter::runGas(Instruction::APPROVE, version), GasMeter::runGas(Instruction::RETURN, version));

	// Operands bottom-up: scope, then length and offset on top. RETURN ignores the scope operand
	// left at the bottom of the stack, so it can be metered with the identical sequence.
	std::vector<u256> const operands{u256(3), u256(64), u256(128)};
	GasMeter::GasConsumption const reference = instructionGas(operands, Instruction::RETURN, version);
	GasMeter::GasConsumption const gas = instructionGas(operands, Instruction::APPROVE, version);
	BOOST_REQUIRE(!reference.isInfinite);
	BOOST_CHECK(!gas.isInfinite);
	BOOST_CHECK_EQUAL(gas.value, reference.value);
}

BOOST_AUTO_TEST_CASE(gas_reporting_opcodes_are_not_deterministic)
{
	// TXPARAM reports the current frame's state_gas_left (param 0x0C) and FRAMEPARAM reports
	// gas_used (params 0x0A/0x0B), which a cross-frame state-gas refill can still reduce for an
	// already completed frame. Both therefore depend on preceding operations and must not be
	// moved or common-subexpression-eliminated, like BALANCE.
	for (auto instruction: {Instruction::TXPARAM, Instruction::FRAMEPARAM})
	{
		BOOST_CHECK(!SemanticInformation::isDeterministic(AssemblyItem(instruction)));
		BOOST_CHECK(!SemanticInformation::movable(instruction));
		BOOST_CHECK(SemanticInformation::otherState(instruction) == SemanticInformation::Read);
	}
}

BOOST_AUTO_TEST_CASE(payload_readers_are_deterministic)
{
	// Frame data and signature entries are fixed for the whole transaction, so reading them is
	// deterministic, exactly like reading calldata.
	for (auto instruction: {
		Instruction::FRAMEDATALOAD,
		Instruction::FRAMEDATACOPY,
		Instruction::SIGPARAM,
		Instruction::SIGDATACOPY,
	})
	{
		BOOST_CHECK(SemanticInformation::isDeterministic(AssemblyItem(instruction)));
		BOOST_CHECK(SemanticInformation::otherState(instruction) == SemanticInformation::None);
	}
	// The copy variants are still not movable, because they write to memory.
	BOOST_CHECK(SemanticInformation::movable(Instruction::FRAMEDATALOAD));
	BOOST_CHECK(SemanticInformation::movable(Instruction::SIGPARAM));
	BOOST_CHECK(!SemanticInformation::movable(Instruction::FRAMEDATACOPY));
	BOOST_CHECK(!SemanticInformation::movable(Instruction::SIGDATACOPY));
}

BOOST_AUTO_TEST_CASE(approve_terminates_control_flow)
{
	BOOST_CHECK(SemanticInformation::terminatesControlFlow(Instruction::APPROVE));
	BOOST_CHECK(SemanticInformation::altersControlFlow(AssemblyItem(Instruction::APPROVE)));
}

BOOST_AUTO_TEST_CASE(read_opcodes_do_not_terminate)
{
	for (auto inst: {
		Instruction::TXPARAM,
		Instruction::FRAMEDATALOAD,
		Instruction::FRAMEDATACOPY,
		Instruction::FRAMEPARAM,
		Instruction::SIGPARAM,
		Instruction::SIGDATACOPY,
	})
	{
		BOOST_CHECK(!SemanticInformation::terminatesControlFlow(inst));
		BOOST_CHECK(!SemanticInformation::altersControlFlow(AssemblyItem(inst)));
	}
}

BOOST_AUTO_TEST_CASE(memory_effects)
{
	BOOST_CHECK(SemanticInformation::memory(Instruction::FRAMEDATACOPY) == SemanticInformation::Write);
	BOOST_CHECK(SemanticInformation::memory(Instruction::SIGDATACOPY) == SemanticInformation::Write);
	BOOST_CHECK(SemanticInformation::memory(Instruction::APPROVE) == SemanticInformation::Read);
	// Pure reads of frame/tx/signature context do not touch memory.
	BOOST_CHECK(SemanticInformation::memory(Instruction::TXPARAM) == SemanticInformation::None);
	BOOST_CHECK(SemanticInformation::memory(Instruction::FRAMEPARAM) == SemanticInformation::None);
	BOOST_CHECK(SemanticInformation::memory(Instruction::FRAMEDATALOAD) == SemanticInformation::None);
	BOOST_CHECK(SemanticInformation::memory(Instruction::SIGPARAM) == SemanticInformation::None);
}

BOOST_AUTO_TEST_CASE(approve_classified_as_state_writer)
{
	BOOST_CHECK(SemanticInformation::otherState(Instruction::APPROVE) == SemanticInformation::Write);
}

BOOST_AUTO_TEST_CASE(view_pure_function_restrictions)
{
	BOOST_CHECK(SemanticInformation::invalidInPureFunctions(Instruction::TXPARAM));
	BOOST_CHECK(SemanticInformation::invalidInPureFunctions(Instruction::FRAMEPARAM));
	BOOST_CHECK(SemanticInformation::invalidInPureFunctions(Instruction::FRAMEDATALOAD));
	BOOST_CHECK(SemanticInformation::invalidInPureFunctions(Instruction::FRAMEDATACOPY));
	BOOST_CHECK(SemanticInformation::invalidInPureFunctions(Instruction::SIGPARAM));
	BOOST_CHECK(SemanticInformation::invalidInPureFunctions(Instruction::SIGDATACOPY));
	BOOST_CHECK(SemanticInformation::invalidInViewFunctions(Instruction::APPROVE));
}

BOOST_AUTO_TEST_CASE(approve_not_movable)
{
	BOOST_CHECK(!SemanticInformation::movable(Instruction::APPROVE));
	BOOST_CHECK(!SemanticInformation::canBeRemoved(Instruction::APPROVE));
}

BOOST_AUTO_TEST_SUITE_END()

} // end namespaces
