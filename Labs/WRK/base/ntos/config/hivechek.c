/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    hivechek.c

Abstract:

    This module implements consistency checking for hives.

--*/

#include    "cmp.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,HvCheckHive)
#pragma alloc_text(PAGE,HvCheckBin)
#endif

//
// debug structures
//
extern struct {
    PHHIVE      Hive;
    ULONG       Status;
    ULONG       Space;
    HCELL_INDEX MapPoint;
    PHBIN       BinPoint;
} HvCheckHiveDebug;

extern struct {
    PHBIN       Bin;
    ULONG       Status;
    PHCELL      CellPoint;
} HvCheckBinDebug;


#if DBG
ULONG HvHiveChecking=0;
#endif

ULONG
HvCheckHive(
    PHHIVE  Hive,
    PULONG  Storage OPTIONAL
    )
/*++

Routine Description:

    Check the consistency of a hive.  Apply CheckBin to bins, make sure
    all pointers in the cell map point to correct places.

Arguments:

    Hive - supplies a pointer to the hive control structure for the
            hive of interest.

    Storage - supplies adddress of ULONG to receive size of allocated user data

Return Value:

    0 if Hive is OK.  Error return indicator if not.  Error value
    comes from one of the check procedures.

    RANGE:  2000 - 2999

--*/
{
    HCELL_INDEX p;
    ULONG       Length;
    ULONG       localstorage = 0;
    PHMAP_ENTRY t;
    PHBIN       Bin = NULL;
    ULONG   i;
    ULONG   rc;
    PFREE_HBIN  FreeBin;

    HvCheckHiveDebug.Hive = Hive;
    HvCheckHiveDebug.Status = 0;
    HvCheckHiveDebug.Space = (ULONG)-1;
    HvCheckHiveDebug.MapPoint = HCELL_NIL;
    HvCheckHiveDebug.BinPoint = 0;

    p = 0;

    //
    // we need to make sure all the cell's data is faulted in inside a 
    // try/except block, as the IO to fault the data in can throw exceptions
    // STATUS_INSUFFICIENT_RESOURCES, in particular
    //

    try {
        //
        // one pass for Stable space, one pass for Volatile
        //
        for (i = 0; i <= Volatile; i++) {
            Length = Hive->Storage[i].Length;

            //
            // for each bin in the space
            //
            while (p < Length) {
                t = HvpGetCellMap(Hive, p);
                if (t == NULL) {
                    CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"HvCheckHive:"));
                    CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"\tBin@:%p invalid\n", Bin));
                    HvCheckHiveDebug.Status = 2005;
                    HvCheckHiveDebug.Space = i;
                    HvCheckHiveDebug.MapPoint = p;
                    return 2005;
                }

            
                if( (t->BinAddress & (HMAP_INPAGEDPOOL|HMAP_INVIEW)) == 0) {
                    //
                    // view is not mapped, neither in paged pool
                    // try to map it.
                    //
                
                    // volatile info is always in paged pool
                    ASSERT( i == Stable );

                    if( !NT_SUCCESS(CmpMapThisBin((PCMHIVE)Hive,p,FALSE)) ) {
                        //
                        // we cannot map this bin due to insufficient resources. 
                        //
                        CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"HvCheckHive:"));
                        CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"\tinsufficient resources while mapping Bin@:%p\n", Bin));
                        HvCheckHiveDebug.Status = 2006;
                        HvCheckHiveDebug.Space = i;
                        HvCheckHiveDebug.MapPoint = p;
                        return 2010;
                    }
                }

                if ((t->BinAddress & HMAP_DISCARDABLE) == 0) {

                    Bin = (PHBIN)HBIN_BASE(t->BinAddress);

                    //
                    // bin header valid?
                    //
                    if ( (Bin->Size > Length)                           ||
                         (Bin->Signature != HBIN_SIGNATURE)             ||
                         (Bin->FileOffset != p)
                       )
                    {
                        CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"HvCheckHive:"));
                        CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"\tBin@:%p invalid\n", Bin));
                        HvCheckHiveDebug.Status = 2010;
                        HvCheckHiveDebug.Space = i;
                        HvCheckHiveDebug.MapPoint = p;
                        HvCheckHiveDebug.BinPoint = Bin;
                        return 2010;
                    }

                    //
                    // structure inside the bin valid?
                    //
                    rc = HvCheckBin(Hive, Bin, &localstorage);
                    if (rc != 0) {
                        HvCheckHiveDebug.Status = rc;
                        HvCheckHiveDebug.Space = i;
                        HvCheckHiveDebug.MapPoint = p;
                        HvCheckHiveDebug.BinPoint = Bin;
                        return rc;
                    }

                    p = (ULONG)p + Bin->Size;

                } else {
                    //
                    // Bin is not present, skip it and advance to the next one.
                    //
                    FreeBin = (PFREE_HBIN)t->BlockAddress;
                    p+=FreeBin->Size;
                }
            }

            p = 0x80000000;     // Beginning of Volatile space
        }

    } except (EXCEPTION_EXECUTE_HANDLER) {
        HvCheckHiveDebug.Status = 2015;
        HvCheckHiveDebug.Space = GetExceptionCode();
        return HvCheckHiveDebug.Status;
    }

    if (ARGUMENT_PRESENT(Storage)) {
        *Storage = localstorage;
    }
    return 0;
}


ULONG
HvCheckBin(
    PHHIVE  Hive,
    PHBIN   Bin,
    PULONG  Storage
    )
/*++

Routine Description:

    Step through all of the cells in the bin.  Make sure that
    they are consistent with each other, and with the bin header.

Arguments:

    Hive - pointer to the hive control structure

    Bin - pointer to bin to check

    Storage - pointer to a ulong to get allocated user data size

Return Value:

    0 if Bin is OK.  Number of test in procedure that failed if not.

    RANGE:  1 - 1999

--*/
{
    PHCELL  p;
    PHCELL  np;
    PHCELL  lp;
    ULONG   freespace = 0L;
    ULONG   allocated = 0L;
    ULONG   userallocated = 0L;

    HvCheckBinDebug.Bin = Bin;
    HvCheckBinDebug.Status = 0;
    HvCheckBinDebug.CellPoint = 0;

    //
    // Scan all the cells in the bin, total free and allocated, check
    // for impossible pointers.
    //
    p = (PHCELL)((PUCHAR)Bin + sizeof(HBIN));
    lp = p;

    // The way allocated and freespace are computed implies the following invariants:
    // 1. allocated + freespace = p + p->Size - (Bin + sizeof(HBIN)). This is because p->Size is added either to allocated or to freespace.
    //    So, assuming that allocated > Bin->Size , then
    //              ==> p + p->Size - (Bin + sizeof(HBIN)) > Bin->Size.
    //              ==> p + p->Size > Bin + Bin->Size + sizeof(HBIN)
    //              ==> p + p->Size > Bin + Bin->Size
    //      This proves that the test "NeverFail 1" (see below) will never fail, because when something is wrong, the test above it (namely "Fail 1") will fail 
    //      and the function will exit.
    //
    //    The same logic applies to the test "NeverFail 2", so it can be removed also.
    //
    // 2. The new value of p is always calculated as p = p + p->Size. By the time this is done, the new value of p (ie. p + p->Size) is already checked against 
    //      Bin + Bin->Size (see tests "Fail 1" and "Fail 2"). So, if p > Bin + Bin->Size, either "Fail 1" or "Fail 2" will fail before assigning the new bogus value 
    //      to p. Therefore, the only possible path to exit the while loop (except a return 20 or return 40), is when p == Bin + Bin->Size.
    //      ==> test "NeverFail 3" can be removed as it will never fail !
    //
    // 3. Considering 1 (where p + p->Size became p) 
    //              ==> allocated + freespace =  p - (Bin + sizeof(HBIN))
    //    But, Considering 2 (above), when the while loop exits, p = Bin + Bin->Size
    //              ==> allocated + freespace = Bin + Bin->Size - (Bin + sizeof(HBIN))
    //              ==> allocated + freespace + sizeof(HBIN) = Bin->Size
    //       This proves that test "NeverFail 4" (see below) will never fail as the expression tested is always true (if the flow of execution reaches the test point).
    //

    while (p < (PHCELL)((PUCHAR)Bin + Bin->Size)) {

        //
        // Check last pointer
        //
        if (USE_OLD_CELL(Hive)) {
            if (lp == p) {
                if (p->u.OldCell.Last != HBIN_NIL) {
                    CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"HvCheckBin 20: First cell has wrong last pointer\n"));
                    CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"Bin = %p\n", Bin));
                    HvCheckBinDebug.Status = 20;
                    HvCheckBinDebug.CellPoint = p;
                    return 20;
                }
            } else {
                if ((PHCELL)(p->u.OldCell.Last + (PUCHAR)Bin) != lp) {
                    CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"HvCheckBin 30: incorrect last pointer\n"));
                    CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"Bin = %p\n", Bin));
                    CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"p = %p\n", (ULONG_PTR)p));
                    HvCheckBinDebug.Status = 30;
                    HvCheckBinDebug.CellPoint = p;
                    return 30;
                }
            }
        }

        
        //
        // Check size
        //
        if (p->Size < 0) {

            //
            // allocated cell
            //

            // Fail 1
            // This test will always fail prior to the failure of the below test
            //
            if ( ((ULONG)(p->Size * -1) > Bin->Size)        ||
#pragma prefast(suppress:12004, "no overflow.")
                 ( (PHCELL)((p->Size * -1) + (PUCHAR)p) >
                   (PHCELL)((PUCHAR)Bin + Bin->Size) )
               )
            {
                CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"HvCheckBin 40: impossible allocation\n"));
                CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"Bin = %p\n", Bin));
                HvCheckBinDebug.Status = 40;
                HvCheckBinDebug.CellPoint = p;
                return 40;
            }
    
            allocated += (p->Size * -1);
            if (USE_OLD_CELL(Hive)) {
                userallocated += (p->Size * -1) - FIELD_OFFSET(HCELL, u.OldCell.u.UserData);
            } else {
                userallocated += (p->Size * -1) - FIELD_OFFSET(HCELL, u.NewCell.u.UserData);
            }

            np = (PHCELL)((PUCHAR)p + (p->Size * -1));

        } else {

            //
            // free cell
            //

            // Fail 2
            // This test will always fail prior to the failure of the below test
            //
            if ( ((ULONG)p->Size > Bin->Size)               ||
                 ( (PHCELL)(p->Size + (PUCHAR)p) >
                   (PHCELL)((PUCHAR)Bin + Bin->Size) ) ||
                 (p->Size == 0) )
            {
                CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"HvCheckBin 60: impossible free block\n"));
                CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"Bin = %p\n", Bin));
                HvCheckBinDebug.Status = 60;
                HvCheckBinDebug.CellPoint = p;
                return 60;
            }
    
            freespace = freespace + p->Size;
            np = (PHCELL)((PUCHAR)p + p->Size);
        }

        lp = p;
        p = np;
    }

    if (ARGUMENT_PRESENT(Storage)) {
        *Storage += userallocated;
    }
    return 0;
}
