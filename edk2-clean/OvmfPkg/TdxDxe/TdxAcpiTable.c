/** @file
  OVMF ACPI QEMU support

  Copyright (c) 2008 - 2014, Intel Corporation. All rights reserved.<BR>

  Copyright (C) 2012-2014, Red Hat, Inc.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/QemuFwCfgLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PcdLib.h>
#include <Library/OrderedCollectionLib.h>
#include <Library/TdxLib.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiTable.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/TdxMailboxLib.h>
#include <Protocol/Cpu.h>
#include <Uefi.h>
#include <TdxAcpiTable.h>

#define HYC3_CPUID_LEAF              0x40000100
#define HYC3_L2_CPUID_STRIDE        2
#define HYC3_L2_ACTIVE_VCPUS        1

STATIC BOOLEAN  mHyc3MadtInstalled = FALSE;

typedef struct {
  UINT8    Type;
  UINT8    Length;
} HYC3_MADT_ENTRY_HEADER;

STATIC
UINT32
Hyc3GetVmIndex (
  VOID
  )
{
  UINT32  Eax;
  UINT32  Ebx;

  AsmCpuid (HYC3_CPUID_LEAF, &Eax, &Ebx, NULL, NULL);
  if ((Ebx != HYC3_L2_CPUID_STRIDE) || (Eax > 1)) {
    return 0;
  }

  return Eax;
}

STATIC
BOOLEAN
Hyc3IsTdpL2Guest (
  VOID
  )
{
  UINT32  Eax;
  UINT32  Ebx;

  AsmCpuid (HYC3_CPUID_LEAF, &Eax, &Ebx, NULL, NULL);
  return (Eax <= 1) && (Ebx == HYC3_L2_CPUID_STRIDE);
}

STATIC
BOOLEAN
Hyc3MadtIdBelongsToVm (
  IN  UINT32   MadtId,
  IN  UINT32   VmIndex,
  IN  BOOLEAN  UsePhysicalApicIds,
  OUT UINT32   *VirtualApicId
  )
{
  UINT32  BaseApicId;

  if (!UsePhysicalApicIds) {
    if (MadtId >= HYC3_L2_ACTIVE_VCPUS) {
      return FALSE;
    }

    *VirtualApicId = MadtId;
    return TRUE;
  }

  BaseApicId = VmIndex * HYC3_L2_CPUID_STRIDE;
  if ((MadtId < BaseApicId) ||
      (MadtId >= BaseApicId + HYC3_L2_ACTIVE_VCPUS)) {
    return FALSE;
  }

  *VirtualApicId = MadtId - BaseApicId;
  return TRUE;
}

STATIC
BOOLEAN
Hyc3MadtHasPhysicalCpuEntriesForVm (
  IN CONST UINT8  *Source,
  IN UINTN        SourceLength,
  IN UINT32       VmIndex
  )
{
  UINTN                         Offset;
  UINTN                         HeaderLength;
  CONST HYC3_MADT_ENTRY_HEADER  *Entry;
  UINT32                        BaseApicId;

  HeaderLength = sizeof (EFI_ACPI_1_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER);
  if (SourceLength < HeaderLength) {
    return FALSE;
  }

  BaseApicId = VmIndex * HYC3_L2_CPUID_STRIDE;
  Offset     = HeaderLength;
  while (Offset + sizeof (HYC3_MADT_ENTRY_HEADER) <= SourceLength) {
    Entry = (CONST HYC3_MADT_ENTRY_HEADER *)(Source + Offset);
    if ((Entry->Length < sizeof (HYC3_MADT_ENTRY_HEADER)) ||
        (Offset + Entry->Length > SourceLength)) {
      break;
    }

    switch (Entry->Type) {
      case EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC:
      {
        CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC_STRUCTURE  *LocalApic;

        LocalApic = (CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC_STRUCTURE *)(Source + Offset);
        if ((LocalApic->ApicId >= BaseApicId) &&
            (LocalApic->ApicId < BaseApicId + HYC3_L2_ACTIVE_VCPUS)) {
          return TRUE;
        }

        break;
      }
      case EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC:
      {
        CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC_STRUCTURE  *LocalX2Apic;

        LocalX2Apic = (CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC_STRUCTURE *)(Source + Offset);
        if ((LocalX2Apic->X2ApicId >= BaseApicId) &&
            (LocalX2Apic->X2ApicId < BaseApicId + HYC3_L2_ACTIVE_VCPUS)) {
          return TRUE;
        }

        break;
      }
      default:
        break;
    }

    Offset += Entry->Length;
  }

  return FALSE;
}

STATIC
UINTN
Hyc3FilterMadtForVm (
  OUT UINT8        *Destination,
  IN  CONST UINT8  *Source,
  IN  UINTN        SourceLength,
  IN  UINT32       VmIndex,
  OUT UINTN        *CpuEntryCount
  )
{
  UINTN                         Offset;
  UINTN                         DestinationOffset;
  UINTN                         HeaderLength;
  CONST HYC3_MADT_ENTRY_HEADER  *Entry;
  UINT32                        VirtualApicId;
  BOOLEAN                       UsePhysicalApicIds;

  *CpuEntryCount = 0;

  HeaderLength = sizeof (EFI_ACPI_1_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER);
  if (SourceLength < HeaderLength) {
    return 0;
  }

  UsePhysicalApicIds = Hyc3MadtHasPhysicalCpuEntriesForVm (Source, SourceLength, VmIndex);
  DEBUG ((
    DEBUG_INFO,
    "HYC3: MADT filter VM %u using %a APIC IDs.\n",
    VmIndex,
    UsePhysicalApicIds ? "physical" : "virtual"
    ));

  CopyMem (Destination, Source, HeaderLength);
  Offset            = HeaderLength;
  DestinationOffset = HeaderLength;

  while (Offset + sizeof (HYC3_MADT_ENTRY_HEADER) <= SourceLength) {
    Entry = (CONST HYC3_MADT_ENTRY_HEADER *)(Source + Offset);
    if ((Entry->Length < sizeof (HYC3_MADT_ENTRY_HEADER)) ||
        (Offset + Entry->Length > SourceLength)) {
      break;
    }

    switch (Entry->Type) {
      case EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC:
      {
        CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC_STRUCTURE  *LocalApic;
        EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC_STRUCTURE        *NewLocalApic;

        LocalApic = (CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC_STRUCTURE *)(Source + Offset);
        if ((*CpuEntryCount < HYC3_L2_ACTIVE_VCPUS) &&
            Hyc3MadtIdBelongsToVm (LocalApic->ApicId, VmIndex, UsePhysicalApicIds, &VirtualApicId)) {
          CopyMem (Destination + DestinationOffset, Source + Offset, Entry->Length);
          NewLocalApic                   = (EFI_ACPI_6_4_PROCESSOR_LOCAL_APIC_STRUCTURE *)(Destination + DestinationOffset);
          NewLocalApic->ApicId           = (UINT8)VirtualApicId;
          NewLocalApic->AcpiProcessorUid = (UINT8)VirtualApicId;
          DestinationOffset             += Entry->Length;
          (*CpuEntryCount)++;
        }

        break;
      }
      case EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC:
      {
        CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC_STRUCTURE  *LocalX2Apic;
        EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC_STRUCTURE        *NewLocalX2Apic;

        LocalX2Apic = (CONST EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC_STRUCTURE *)(Source + Offset);
        if ((*CpuEntryCount < HYC3_L2_ACTIVE_VCPUS) &&
            Hyc3MadtIdBelongsToVm (LocalX2Apic->X2ApicId, VmIndex, UsePhysicalApicIds, &VirtualApicId)) {
          CopyMem (Destination + DestinationOffset, Source + Offset, Entry->Length);
          NewLocalX2Apic                   = (EFI_ACPI_6_4_PROCESSOR_LOCAL_X2APIC_STRUCTURE *)(Destination + DestinationOffset);
          NewLocalX2Apic->X2ApicId         = VirtualApicId;
          NewLocalX2Apic->AcpiProcessorUid = VirtualApicId;
          DestinationOffset               += Entry->Length;
          (*CpuEntryCount)++;
        }

        break;
      }
      case EFI_ACPI_6_4_LOCAL_APIC_NMI:
      case EFI_ACPI_6_4_LOCAL_X2APIC_NMI:
        // HYC3 L2 guests do not have complete NMI/LINT virtualization yet.
        // Dropping these entries prevents Linux from configuring LINT1 as NMI.
        DEBUG ((DEBUG_INFO, "HYC3: dropped MADT local APIC NMI entry for VM %u.\n", VmIndex));
        break;
      default:
        CopyMem (Destination + DestinationOffset, Source + Offset, Entry->Length);
        DestinationOffset += Entry->Length;
        break;
    }

    Offset += Entry->Length;
  }

  ((EFI_ACPI_DESCRIPTION_HEADER *)Destination)->Length = (UINT32)DestinationOffset;
  return DestinationOffset;
}

STATIC
VOID
Hyc3UpdateAcpiChecksum (
  IN OUT EFI_ACPI_DESCRIPTION_HEADER  *Header
  )
{
  Header->Checksum = 0;
  Header->Checksum = (UINT8)(0 - CalculateSum8 ((CONST UINT8 *)Header, Header->Length));
}

/**
  At the beginning of system boot, a 4K-aligned, 4K-size memory (Td mailbox) is
  pre-allocated by host VMM. BSP & APs do the page accept together in that memory
  region.

  After that TDVF is designed to relocate the mailbox to a 4K-aligned, 4K-size
  memory block which is allocated in the ACPI Nvs memory. APs are waken up and
  spin around the relocated mailbox for further command.

  @return   EFI_PHYSICAL_ADDRESS    Address of the relocated mailbox
**/
EFI_PHYSICAL_ADDRESS
EFIAPI
RelocateMailbox (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Address;
  VOID                  *ApLoopFunc;
  UINT32                RelocationPages;
  MP_RELOCATION_MAP     RelocationMap;
  MP_WAKEUP_MAILBOX     *RelocatedMailBox;
  EFI_STATUS            Status;

  Address    = 0;
  ApLoopFunc = NULL;
  ZeroMem (&RelocationMap, sizeof (RelocationMap));

  //
  // Get information needed to setup aps running in their
  // run loop in allocated acpi reserved memory
  // Add another page for mailbox
  //
  AsmGetRelocationMap (&RelocationMap);
  if ((RelocationMap.RelocateApLoopFuncAddress == 0) || (RelocationMap.RelocateApLoopFuncSize == 0)) {
    DEBUG ((DEBUG_ERROR, "Failed to get the RelocationMap.\n"));
    return 0;
  }

  RelocationPages = EFI_SIZE_TO_PAGES ((UINT32)RelocationMap.RelocateApLoopFuncSize) + 1;

  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIMemoryNVS, RelocationPages, &Address);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to allocate pages for MailboxRelocation. %r\n", Status));
    return 0;
  }

  ZeroMem ((VOID *)Address, EFI_PAGES_TO_SIZE (RelocationPages));

  ApLoopFunc = (VOID *)((UINTN)Address + EFI_PAGE_SIZE);

  CopyMem (
    ApLoopFunc,
    RelocationMap.RelocateApLoopFuncAddress,
    RelocationMap.RelocateApLoopFuncSize
    );

  DEBUG ((
    DEBUG_INFO,
    "Ap Relocation: mailbox %llx, loop %p\n",
    Address,
    ApLoopFunc
    ));

  //
  // Initialize mailbox
  //
  RelocatedMailBox               = (MP_WAKEUP_MAILBOX *)Address;
  RelocatedMailBox->Command      = MpProtectedModeWakeupCommandNoop;
  RelocatedMailBox->ApicId       = MP_CPU_PROTECTED_MODE_MAILBOX_APICID_INVALID;
  RelocatedMailBox->WakeUpVector = 0;

  //
  // Wakup APs and have been move to the finalized run loop
  // They will spin until guest OS wakes them
  //
  MpSerializeStart ();

  MpSendWakeupCommand (
    MpProtectedModeWakeupCommandWakeup,
    (UINT64)ApLoopFunc,
    (UINT64)RelocatedMailBox,
    0,
    0,
    0
    );

  return Address;
}

/**
  Alter the MADT when ACPI Table from QEMU is available.

  @param[in]  Event     Event whose notification function is being invoked
  @param[in]  Context   Pointer to the notification function's context
**/
VOID
EFIAPI
AlterAcpiTable (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_ACPI_SDT_PROTOCOL                                *AcpiSdtProtocol;
  EFI_ACPI_TABLE_PROTOCOL                              *AcpiTableProtocol;
  EFI_STATUS                                           Status;
  UINTN                                                Index;
  EFI_ACPI_SDT_HEADER                                  *Table;
  EFI_ACPI_TABLE_VERSION                               Version;
  UINTN                                                OriginalTableKey;
  UINTN                                                NewTableKey;
  UINT8                                                *NewMadtTable;
  UINTN                                                NewMadtTableLength;
  UINTN                                                FilteredMadtTableLength;
  UINTN                                                FilteredCpuCount;
  UINT32                                               VmIndex;
  EFI_PHYSICAL_ADDRESS                                 RelocateMailboxAddress;
  EFI_ACPI_6_4_MULTIPROCESSOR_WAKEUP_STRUCTURE         *MadtMpWk;
  EFI_ACPI_1_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  *MadtHeader;
  BOOLEAN                                              IsHyc3TdpL2;

  if (mHyc3MadtInstalled) {
    DEBUG ((DEBUG_INFO, "HYC3: MADT already filtered, skipping duplicate request\n"));
    return;
  }

  Index                  = 0;
  NewMadtTable           = NULL;
  MadtHeader             = NULL;
  MadtMpWk               = NULL;
  FilteredCpuCount       = 0;
  IsHyc3TdpL2            = Hyc3IsTdpL2Guest ();
  RelocateMailboxAddress = 0;

  Status = gBS->LocateProtocol (&gEfiAcpiSdtProtocolGuid, NULL, (void **)&AcpiSdtProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Unable to locate ACPI SDT protocol.\n"));
    return;
  }

  if (!IsHyc3TdpL2) {
    RelocateMailboxAddress = RelocateMailbox ();
    if (RelocateMailboxAddress == 0) {
      ASSERT (FALSE);
      DEBUG ((DEBUG_ERROR, "Failed to relocate Td mailbox\n"));
      return;
    }
  } else {
    DEBUG ((DEBUG_INFO, "HYC3: TDP L2 guest, filtering MADT without TDX mailbox relocation\n"));
  }

  do {
    Status = AcpiSdtProtocol->GetAcpiTable (Index, &Table, &Version, &OriginalTableKey);

    if (!EFI_ERROR (Status) && (Table->Signature == EFI_ACPI_1_0_APIC_SIGNATURE)) {
      Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (void **)&AcpiTableProtocol);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Unable to locate ACPI Table protocol.\n"));
        break;
      }

      VmIndex            = Hyc3GetVmIndex ();
      NewMadtTableLength = Table->Length;
      if (!IsHyc3TdpL2) {
        NewMadtTableLength += sizeof (EFI_ACPI_6_4_MULTIPROCESSOR_WAKEUP_STRUCTURE);
      }

      NewMadtTable = AllocatePool (NewMadtTableLength);
      if (NewMadtTable == NULL) {
        DEBUG ((DEBUG_ERROR, "%a: OUT_OF_SOURCES error.\n", __func__));
        break;
      }

      if (IsHyc3TdpL2) {
        FilteredMadtTableLength = Hyc3FilterMadtForVm (NewMadtTable, (CONST UINT8 *)Table, Table->Length, VmIndex, &FilteredCpuCount);
        if (FilteredMadtTableLength == 0) {
          DEBUG ((DEBUG_ERROR, "%a: failed to filter MADT for VM %u.\n", __func__, VmIndex));
          break;
        }
      } else {
        CopyMem (NewMadtTable, (UINT8 *)Table, Table->Length);
        FilteredMadtTableLength = Table->Length;
      }

      NewMadtTableLength        = FilteredMadtTableLength;
      MadtHeader                = (EFI_ACPI_1_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER *)NewMadtTable;
      MadtHeader->Header.Length = (UINT32)NewMadtTableLength;

      if (!IsHyc3TdpL2) {
        NewMadtTableLength       += sizeof (EFI_ACPI_6_4_MULTIPROCESSOR_WAKEUP_STRUCTURE);
        MadtHeader->Header.Length = (UINT32)NewMadtTableLength;
        MadtMpWk                 = (EFI_ACPI_6_4_MULTIPROCESSOR_WAKEUP_STRUCTURE *)(NewMadtTable + FilteredMadtTableLength);
        MadtMpWk->Type           = EFI_ACPI_6_4_MULTIPROCESSOR_WAKEUP;
        MadtMpWk->Length         = sizeof (EFI_ACPI_6_4_MULTIPROCESSOR_WAKEUP_STRUCTURE);
        MadtMpWk->MailBoxVersion = 0;
        MadtMpWk->Reserved       = 0;
        MadtMpWk->MailBoxAddress = RelocateMailboxAddress;
      }

      Hyc3UpdateAcpiChecksum (&MadtHeader->Header);

      DEBUG ((
        DEBUG_INFO,
        "HYC3: installed MADT for VM %u, tdp_l2 %u, old length 0x%x, new length 0x%x, cpu entries %u.\n",
        VmIndex,
        IsHyc3TdpL2 ? 1 : 0,
        Table->Length,
        (UINT32)NewMadtTableLength,
        (UINT32)FilteredCpuCount
        ));

      Status = AcpiTableProtocol->InstallAcpiTable (AcpiTableProtocol, NewMadtTable, NewMadtTableLength, &NewTableKey);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Failed to install new MADT table. %r\n", Status));
        break;
      }

      Status = AcpiTableProtocol->UninstallAcpiTable (AcpiTableProtocol, OriginalTableKey);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Uninstall old MADT table error.\n"));
      }

      mHyc3MadtInstalled = TRUE;
      break;
    }

    Index++;
  } while (!EFI_ERROR (Status));

  if (NewMadtTable != NULL) {
    FreePool (NewMadtTable);
  }
}
