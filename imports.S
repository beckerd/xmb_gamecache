	.set noreorder

#include "pspstub.s"

// weak stub: present on 6.60/6.61, only used for OPNSSMP games
	STUB_START "SysMemForKernel", 0x00010011, 0x000010005
	STUB_FUNC 0xCD617A94, sceKernelGetGameInfo
	STUB_END

#include "pspimport.s"

	IMPORT_START "SystemCtrlForKernel",0x00090000
	IMPORT_FUNC  "SystemCtrlForKernel",0x159AF5CC,sctrlHENFindFunction
	IMPORT_FUNC  "SystemCtrlForKernel",0xF988C1DC,sctrlHENPatchSyscall
	IMPORT_FUNC  "SystemCtrlForKernel",0x1C90BECB,sctrlHENSetStartModuleHandler
	IMPORT_FUNC  "SystemCtrlForKernel",0x72F47790,sctrlHookImportByNID
	IMPORT_FUNC  "SystemCtrlForKernel",0x2D10FB28,sctrlKernelLoadExecVSHWithApitype
	IMPORT_FUNC  "SystemCtrlForKernel",0x5A35C948,sctrlSESetUmdFile
	IMPORT_FUNC  "SystemCtrlForKernel",0x31C6160D,sctrlSESetDiscType
	IMPORT_FUNC  "SystemCtrlForKernel",0xBC939DC1,sctrlSESetBootConfFileIndex
	IMPORT_FUNC  "SystemCtrlForKernel",0x1090A2E1,sctrlHENGetVersion
	IMPORT_FUNC  "SystemCtrlForKernel",0x5328B431,sctrlHENGetMinorVersion
	IMPORT_FUNC  "SystemCtrlForKernel",0x16100529,LZ4_decompress_fast
	IMPORT_FUNC  "SystemCtrlForKernel",0x76C382FF,lzo1x_decompress
