#include "BinFormat.h"
#include "TextFormat.h"

#include "FileLog.h"

// annotated tracer dependencies
#include "SymbolicEnvironment/Environment.h"
#include "TrackingExecutor.h"
#include "annotated.tracer.h"

#ifdef _WIN32
#include <Windows.h>
#define GET_LIB_HANDLER2(libname) LoadLibraryA((libname))
#else
#define GET_LIB_HANDLER2(libname) dlopen((libname), RTLD_LAZY)

#endif
#define MAX_BUFF 4096


// setup static values for BitMap and TrackingExecutor

int BitMap::instCount = 0;

const unsigned int TrackingExecutor::flagValues[] = {
	RIVER_SPEC_FLAG_OF, RIVER_SPEC_FLAG_OF,
	RIVER_SPEC_FLAG_CF, RIVER_SPEC_FLAG_CF,
	RIVER_SPEC_FLAG_ZF, RIVER_SPEC_FLAG_ZF,
	RIVER_SPEC_FLAG_ZF | RIVER_SPEC_FLAG_CF, RIVER_SPEC_FLAG_ZF | RIVER_SPEC_FLAG_CF,
	RIVER_SPEC_FLAG_SF, RIVER_SPEC_FLAG_SF,
	RIVER_SPEC_FLAG_PF, RIVER_SPEC_FLAG_PF,
	RIVER_SPEC_FLAG_OF | RIVER_SPEC_FLAG_SF, RIVER_SPEC_FLAG_OF | RIVER_SPEC_FLAG_SF,
	RIVER_SPEC_FLAG_OF | RIVER_SPEC_FLAG_SF | RIVER_SPEC_FLAG_ZF, RIVER_SPEC_FLAG_OF | RIVER_SPEC_FLAG_SF | RIVER_SPEC_FLAG_ZF,
};

typedef void __stdcall SH(void *, void *, void *);

void AddReference(void *ref) {
	((BitMap *)ref)->AddRef();
}

void DelReference(void *ref) {
	((BitMap *)ref)->DelRef();
}

namespace at {

unsigned int CustomObserver::ExecutionBegin(void *ctx, void *address) {
	printf("Process starting\n");
	at->ctrl->GetModules(mInfo, mCount);

	if (!patchFile.empty()) {
		std::ifstream fPatch;

		fPatch.open(patchFile);

		if (!fPatch.good()) {
			std::cout << "Patch file not found" << std::endl;
			return EXECUTION_TERMINATE;
		}

		PatchLibrary(fPatch);
		fPatch.close();
	}

	if (!ctxInit) {
		at->bitMapZero = new BitMap(at->varCount, 1);

		revEnv = NewX86RevtracerEnvironment(ctx, at->ctrl); //new RevSymbolicEnvironment(ctx, at->ctrl);
		regEnv = NewX86RegistersEnvironment(revEnv); //new OverlappedRegistersEnvironment();
		//TODO: is this legit?
		executor = new TrackingExecutor(regEnv, at);
		regEnv->SetExecutor(executor);
		regEnv->SetReferenceCounting(AddReference, DelReference);

		for (unsigned int i = 0; i < at->varCount; ++i) {
			char vname[8];

			sprintf(vname, "s[%d]", i);

			revEnv->SetSymbolicVariable(vname,
					(rev::ADDR_TYPE)(&at->payloadBuff[i]), 1);
		}

		ctxInit = true;
	}

	aFormat->WriteTestName(fileName.c_str());

	return EXECUTION_ADVANCE;
}

unsigned int CustomObserver::ExecutionControl(void *ctx, void *address) {
	rev::BasicBlockInfo bbInfo;
	at->ctrl->GetLastBasicBlockInfo(ctx, &bbInfo);

	const char unkmod[MAX_PATH] = "???";
	unsigned int offset = (DWORD)bbInfo.address;
	int foundModule = -1;

	for (int i = 0; i < mCount; ++i) {
		if ((mInfo[i].ModuleBase <= (DWORD)bbInfo.address) && ((DWORD)bbInfo.address < mInfo[i].ModuleBase + mInfo[i].Size)) {
			offset -= mInfo[i].ModuleBase;
			foundModule = i;
			break;
		}
	}

	if (executor->condCount) {
		for (unsigned int i = 0; i < at->varCount; ++i) {
			bool bPrint = true;
			for (unsigned int j = 0; j < executor->condCount; ++j) {
				bPrint &= ((BitMap *)executor->lastCondition[j])->GetBit(i);
			}

			if (bPrint) {
				aFormat->WriteInputUsage(i);
			}
		}
	}

	for (unsigned int i = 0; i < executor->condCount; ++i) {
		executor->lastCondition[i]->DelRef();
		executor->lastCondition[i] = nullptr;
	}
	executor->condCount = 0;

	aFormat->WriteBasicBlock(
			(-1 == foundModule) ? unkmod : mInfo[foundModule].Name,
			offset,
			bbInfo.cost,
			0
			);
	return EXECUTION_ADVANCE;
}

unsigned int CustomObserver::ExecutionEnd(void *ctx) {
	if (at->batched) {
		CorpusItemHeader header;
		if ((1 == fread(&header, sizeof(header), 1, stdin)) &&
				(header.size == fread(at->payloadBuff, 1, header.size, stdin))) {
			std::cout << "Using " << header.fName << " as input file." << std::endl;

			aFormat->WriteTestName(header.fName);
			return EXECUTION_RESTART;
		}

		return EXECUTION_TERMINATE;
	} else {
		return EXECUTION_TERMINATE;
	}
}

unsigned int CustomObserver::TranslationError(void *ctx, void *address) {
	printf("Error issued at address %p\n", address);
	auto direction = ExecutionEnd(ctx);
	if (direction == EXECUTION_RESTART) {
		printf("Restarting after issue\n");
	}
	printf("Translation error. Exiting ...\n");
	exit(1);
	return direction;
}

CustomObserver::CustomObserver(AnnotatedTracer *at) {
	ctxInit = false;

	regEnv = nullptr;
	revEnv = nullptr;

	this->at = at;
}

CustomObserver::~CustomObserver() {
}

unsigned int AnnotatedTracer::ComputeVarCount() {
	if (payloadBuff == nullptr)
		return 1;

	char *buff = payloadBuff;
	unsigned int bSize = MAX_BUFF;
	do {
		fgets(buff, bSize, stdin);
		while (*buff) {
			buff++;
			bSize--;
		}
	} while (!feof(stdin));

	return buff - payloadBuff - 1;
}

void AnnotatedTracer::SetSymbolicHandler(rev::SymbolicHandlerFunc symb) {
	this->symb = symb;
}

AnnotatedTracer::AnnotatedTracer()
	: observer(this), batched(false), ctrl(nullptr), varCount(1),
	payloadBuff(nullptr), PayloadHandler(nullptr), bitMapZero(nullptr)
{ }

AnnotatedTracer::~AnnotatedTracer()
{}

int AnnotatedTracer::Run(ez::ezOptionParser &opt) {
	uint32_t executionType = EXECUTION_INPROCESS;

	if (opt.isSet("--extern")) {
		executionType = EXECUTION_EXTERNAL;
	}

	ctrl = NewExecutionController(executionType);

	std::string fModule;
	opt.get("-p")->getString(fModule);
	std::cout << "Using payload " << fModule << std::endl;
	if (executionType == EXECUTION_EXTERNAL)
		std::cout << "Starting " << ((executionType == EXECUTION_EXTERNAL) ? "extern" : "internal") << " tracing on module " << fModule << "\n";

	if (executionType == EXECUTION_INPROCESS) {
		LIB_T hModule = GET_LIB_HANDLER2(fModule.c_str());
		if (nullptr == hModule) {
			std::cout << "Payload not found" << std::endl;
			return 0;
		}

		payloadBuff = (char *)LOAD_PROC(hModule, "payloadBuffer");
		PayloadHandler = (PayloadHandlerFunc)LOAD_PROC(hModule, "Payload");

		if ((nullptr == payloadBuff) || (nullptr == PayloadHandler)) {
			std::cout << "Payload imports not found" << std::endl;
			return 0;
		}
	}


	if (opt.isSet("--binlog")) {
		observer.binOut = true;
	}

	std::string fName;
	opt.get("-o")->getString(fName);
	std::cout << "Writing " << (observer.binOut ? "binary" : "text") << " output to " << fName << std::endl;

	FileLog *flog = new FileLog();
	flog->SetLogFileName(fName.c_str());
	observer.aLog = flog;

	if (observer.binOut) {
		observer.aFormat = new BinFormat(observer.aLog);
	} else {
		observer.aFormat = new TextFormat(observer.aLog);
	}

	varCount = ComputeVarCount();

	if (opt.isSet("-m")) {
		opt.get("-m")->getString(observer.patchFile);
	}

	if (executionType == EXECUTION_INPROCESS) {
		ctrl->SetEntryPoint((void*)PayloadHandler);
	} else if (executionType == EXECUTION_EXTERNAL) {
		wchar_t ws[4096];
		std::mbstowcs(ws, fModule.c_str(), fModule.size() + 1);
		std::cout << "Converted module name [" << fModule << "] to wstring [";
		std::wcout << std::wstring(ws) << "]\n";
		ctrl->SetPath(std::wstring(ws));
	}

	ctrl->SetExecutionFeatures(TRACER_FEATURE_SYMBOLIC);

	ctrl->SetExecutionObserver(&observer);
	ctrl->SetSymbolicHandler(symb);

	if (opt.isSet("--batch")) {
		batched = true;
		freopen(NULL, "rb", stdin);

		while (!feof(stdin)) {
			CorpusItemHeader header;
			if ((1 == fread(&header, sizeof(header), 1, stdin)) &&
					(header.size == fread(payloadBuff, 1, header.size, stdin))) {
				std::cout << "Using " << header.fName << " as input file." << std::endl;

				observer.fileName = header.fName;

				ctrl->Execute();
				ctrl->WaitForTermination();
			}
		}

	} else {
		char *buff = payloadBuff;
		unsigned int bSize = MAX_BUFF;
		do {
			fgets(buff, bSize, stdin);
			while (*buff) {
				buff++;
				bSize--;
			}
		} while (!feof(stdin));

		observer.fileName = "stdin";

		ctrl->Execute();
		ctrl->WaitForTermination();
	}

	DeleteExecutionController(ctrl);
	ctrl = NULL;

	return 0;
}

} // namespace at
