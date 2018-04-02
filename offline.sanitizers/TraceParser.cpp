#include "TraceParser.h"
#include "Z3Handler.h"
#undef FLAG_LEN
#include "BinFormat.h"

#include "revtracer/river.h"
#include "CommonCrossPlatform/Common.h"

#include <string.h>
#include <stdlib.h>

#define MAX_BUFF (1 << 15)

TraceParser::TraceParser()
	: z3Handler()
{
	state = NONE;
}

TraceParser::~TraceParser() {}

/*
 * Z3 logs come in the following order:
 * 0. z3 module name (only for z3 address)
 * 1. z3 address or jump
 * 2. z3 ast
 * 3. bare trace entry - metadata
 */
bool TraceParser::Parse(FILE *input) {
	int ret, next = 0, symbolicType;
	// binformat structs
	BinLogEntryHeader *bleh;
	BinLogEntry *ble;
	unsigned char *buff = (unsigned char *) malloc (MAX_BUFF * sizeof(unsigned char));
	if (buff == nullptr)
		DEBUG_BREAK;

	// traceparser structs
	struct BasicBlock bb;
	struct AddressAssertion addrAssertion;
	struct JccCondition jccCond;
	memset(&bb, 0, sizeof(bb));
	memset(&addrAssertion, 0, sizeof(addrAssertion));
	memset(&jccCond, 0, sizeof(jccCond));

	while (!feof(input)) {
		ret = ReadFromStream(buff, sizeof(*bleh), input);
		if (ret < 0) {
			DEBUG_BREAK;
		} else if (ret == 0) {
			break;
		}

		PrintState();

		bleh = (BinLogEntryHeader *)buff;
		DebugPrint(bleh->entryType);

		ret = ReadFromStream(buff + ret, bleh->entryLength, input);
		if (ret <= 0)
			DEBUG_BREAK;

		ble = (BinLogEntry *)buff;

		switch(bleh->entryType) {
			case ENTRY_TYPE_TEST_NAME:
			case ENTRY_TYPE_INPUT_USAGE:
			case ENTRY_TYPE_TAINTED_INDEX:
			case ENTRY_TYPE_BB_MODULE:
				if (state != NONE) {
					DEBUG_BREAK;
				}
				break;
			case ENTRY_TYPE_BB_OFFSET:
				if (state == NONE) {
					break;
				} else if (state == Z3_OFFSET) {
					// if `next` not sent for previous bb
					state = NONE;
					break;
				}

				if (state < Z3_AST)
					DEBUG_BREAK;
				state = Z3_OFFSET;

				switch(symbolicType) {
					case Z3_SYMBOLIC_TYPE_ADDRESS:
						bb.assertionData.asAddress.esp = ble->data.asBBOffset.esp;
						break;
					case Z3_SYMBOLIC_TYPE_JCC:
						bb.assertionData.asJcc.jumpType = ble->data.asBBOffset.jumpType;
						bb.assertionData.asJcc.jumpInstruction = ble->data.asBBOffset.jumpInstruction;
						break;
					default:
						DEBUG_BREAK;
				}
				break;
			case ENTRY_TYPE_BB_NEXT_OFFSET:
				if (state == NONE) {
					break;
				}

				if (state != Z3_OFFSET)
					DEBUG_BREAK;

				if (next > 1) {
					DEBUG_BREAK;
				}

				bb.assertionData.asJcc.next[next++].offset = ble->data.asBBNextOffset.offset;
				if (next > 1) {
					next = 0;
					state = NONE;
				}

				break;
			case ENTRY_TYPE_BB_NEXT_MODULE:
				if (state == NONE) {
					break;
				}

				if (state != Z3_OFFSET)
					DEBUG_BREAK;
				// TODO
				break;
			case ENTRY_TYPE_Z3_MODULE:
				if (state != NONE)
					DEBUG_BREAK;
				state = Z3_MODULE;

				strncpy(bb.current.module,
						(char *)&ble->data, bleh->entryLength);
				bb.current.module[bleh->entryLength + 1] = 0;
				break;
			case ENTRY_TYPE_Z3_SYMBOLIC:
				if (!(state == NONE || state == Z3_MODULE))
					DEBUG_BREAK;
				state = Z3_SYMBOLIC_OBJECT;

				bb.current.offset = ble->data.asZ3Symbolic.source.z3SymbolicAddress.offset;
				symbolicType = ble->data.asZ3Symbolic.header.entryType;

				switch(symbolicType) {
					case Z3_SYMBOLIC_TYPE_ADDRESS:
						addrAssertion.offset = ble->data.asZ3Symbolic.source.z3SymbolicAddress.offset;
						addrAssertion.symbolicBase = ble->data.asZ3Symbolic.source.z3SymbolicAddress.symbolicBase;
						addrAssertion.scale= ble->data.asZ3Symbolic.source.z3SymbolicAddress.scale;
						addrAssertion.symbolicIndex= ble->data.asZ3Symbolic.source.z3SymbolicAddress.symbolicIndex;
						addrAssertion.displacement = ble->data.asZ3Symbolic.source.z3SymbolicAddress.displacement;
						addrAssertion.input = ble->data.asZ3Symbolic.source.z3SymbolicAddress.input;
						addrAssertion.output = ble->data.asZ3Symbolic.source.z3SymbolicAddress.output;
						break;
					case Z3_SYMBOLIC_TYPE_JCC:
						jccCond.testFlags = ble->data.asZ3Symbolic.source.z3SymbolicJumpCC.testFlags;
						jccCond.condition = ble->data.asZ3Symbolic.source.z3SymbolicJumpCC.symbolicCond;

						for (int i = 0; i < FLAG_LEN; ++i) {
							jccCond.symbolicFlags[i] = ble->data.asZ3Symbolic.source.z3SymbolicJumpCC.symbolicFlags[i];
						}
						break;
					default:
						DEBUG_BREAK;
				}
				break;
			case ENTRY_TYPE_Z3_AST:
				if (state != Z3_SYMBOLIC_OBJECT)
					DEBUG_BREAK;
				state = Z3_AST;

				Z3_ast ast;
				ret = SmtToAst(ast, (char *)&ble->data, bleh->entryLength);

				switch(symbolicType) {
					case Z3_SYMBOLIC_TYPE_ADDRESS:
						addrAssertion.symbolicAddress = ast;
						addrAssertion.basicBlock = bb;

						DebugPrint(addrAssertion);
						addrAssertions.push_back(addrAssertion);
						memset(&addrAssertion, 0, sizeof(addrAssertion));
						break;
					case Z3_SYMBOLIC_TYPE_JCC:
						jccCond.symbolicCondition = ast;
						jccCond.basicBlock = bb;

						DebugPrint(jccCond);
						jccConditions.push_back(jccCond);
						memset(&jccCond, 0, sizeof(jccCond));
						break;
					default:
						DEBUG_BREAK;
				}
				memset(&bb, 0, sizeof(bb));
				break;
			default:
				DEBUG_BREAK;

		}
	}
	free(buff);
}

int TraceParser::ReadFromStream(unsigned char* buff, size_t size, FILE *input) {
	if (size >= MAX_BUFF)
		DEBUG_BREAK;

	memset(buff, 0, MAX_BUFF);
	return fread(buff, 1, size, input);
}

int TraceParser::SmtToAst(Z3_ast &ast, char *smt, size_t size) {
	ast = z3Handler.toAst(smt, size);
	if (ast  == nullptr)
		return false;

	return true;
}

void TraceParser::DebugPrint(unsigned type) {
	printf("Received data: %04X<", (unsigned short)type);
	switch(type) {
	case ENTRY_TYPE_Z3_MODULE:
		printf("Z3 module name");
		break;
	case ENTRY_TYPE_Z3_SYMBOLIC:
		printf("Z3 symbolic data");
		break;
	case ENTRY_TYPE_Z3_AST:
		printf("Z3 ast");
		break;
	default:
		printf("unk");
		break;
	}

	printf(">\n");

}

static const char flagNames[FLAG_LEN][3] = {"CF", "PF", "AF", "ZF", "SF",
"OF", "DF"};

void TraceParser::PrintJump(unsigned short jumpType, unsigned short jumpInstruction) {
	switch(jumpType) {
		case RIVER_JUMP_TYPE_IMM:
			printf("type imm;");
			break;
		case RIVER_JUMP_TYPE_MEM:
			printf("type mem;");
			break;
		case RIVER_JUMP_TYPE_REG:
			printf("type reg;");
			break;
		default:
			printf("0x%02X;", jumpType);
	}
	printf(" ");

	switch(jumpInstruction) {
		case RIVER_JUMP_INSTR_RET:
			printf("instr ret;");
			break;
		case RIVER_JUMP_INSTR_JMP:
			printf("instr jump;");
			break;
		case RIVER_JUMP_INSTR_JXX:
			printf("instr jxx;");
			break;
		case RIVER_JUMP_INSTR_CALL:
			printf("instr call;");
			break;
		case RIVER_JUMP_INSTR_SYSCALL:
			printf("instr syscall;");
			break;
		default:
			printf("0x%02X;", jumpInstruction);
	}
}

void TraceParser::DebugPrint(const struct JccCondition &jccCondition) {
	printf("JccCondition %d: %s + 0x%08X\n", jccConditions.size(),
			jccCondition.basicBlock.current.module,
			jccCondition.basicBlock.current.offset);
	printf("JccCondition : %p <=", (void *)jccCondition.condition);
	for (int i = 0; i < FLAG_LEN; ++i) {
		if (jccCondition.testFlags & (1 << i)) {
			printf("%s[%p]", flagNames[i],
					(void *)jccCondition.symbolicFlags[i]);
		}
	}
	printf("\n");
	printf("JccCondition: ");
	PrintJump(jccCondition.basicBlock.assertionData.asJcc.jumpType,
			jccCondition.basicBlock.assertionData.asJcc.jumpInstruction);
	printf("\n");
	for (int i = 0; i < 2; ++i) {
		printf("JccCondition next[%d]: %s + 0x%08X\n", i,
				jccCondition.basicBlock.assertionData.asJcc.next[i].module,
				jccCondition.basicBlock.assertionData.asJcc.next[i].offset);
	}

}

void TraceParser::DebugPrint(const struct AddressAssertion &addrAssertion) {
	printf("AddressAssertion %d: + 0x%08X\n", addrAssertions.size(),
			addrAssertion.offset);
	printf("AddressAssertion %p <= %p + %d * %p + %d\n",
			(void *)addrAssertion.symbolicAddress,
			(void *)addrAssertion.symbolicBase,
			addrAssertion.scale,
			(void *)addrAssertion.symbolicIndex,
			addrAssertion.displacement);
	printf("AddressAssertion i[%d]/o[%d]\n", addrAssertion.input,
			addrAssertion.output);

	printf("AddressAssertion bb: %s + 0x%0xlx esp: 0x%08X\n",
			addrAssertion.basicBlock.current.module,
			addrAssertion.basicBlock.current.offset,
			addrAssertion.basicBlock.assertionData.asAddress.esp);
}

void TraceParser::PrintState() {
	printf("state: ");
	switch(state) {
		case NONE:
			printf("NONE");
			break;
		case Z3_MODULE:
			printf("Z3_MODULE");
			break;
		case Z3_SYMBOLIC_OBJECT:
			printf("Z3_SYMBOLIC_OBJECT");
			break;
		case Z3_AST:
			printf("Z3_AST");
			break;
		case Z3_OFFSET:
			printf("Z3_OFFSET");
			break;
		default:
			DEBUG_BREAK;
	}
	printf("\n");
}
