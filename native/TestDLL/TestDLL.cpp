// TestDLL.cpp : Defines the exported functions for the DLL.
//

#include "pch.h"
#include "framework.h"
#include "TestDLL.h"


// This is an example of an exported variable
TESTDLL_API int nTestDLL=0;

// This is an example of an exported function.
TESTDLL_API int fnTestDLL(void)
{
    return 0;
}

// This is the constructor of a class that has been exported.
CTestDLL::CTestDLL()
{
    return;
}
