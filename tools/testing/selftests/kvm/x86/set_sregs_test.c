// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM_SET_SREGS tests
 *
 * Copyright (C) 2018, Google LLC.
 *
 * This is a regression test for the bug fixed by the following commit:
 * d3802286fa0f ("kvm: x86: Disallow illegal IA32_APIC_BASE MSR values")
 *
 * That bug allowed a user-mode program that called the KVM_SET_SREGS
 * ioctl to put a VCPU's local APIC into an invalid state.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "test_util.h"

#include "kvm_util.h"
#include "processor.h"

#define TEST_INVALID_CR_BIT(vcpu, cr, orig, bit)				\
do {										\
	struct kvm_sregs new;							\
	int rc;									\
										\
	/* Skip the sub-test, the feature/bit is supported. */			\
	if (orig.cr & bit)							\
		break;								\
										\
	memcpy(&new, &orig, sizeof(sregs));					\
	new.cr |= bit;								\
										\
	rc = _vcpu_sregs_set(vcpu, &new);					\
	TEST_ASSERT(rc, "KVM allowed invalid " #cr " bit (0x%lx)", bit);	\
										\
	/* Sanity check that KVM didn't change anything. */			\
	vcpu_sregs_get(vcpu, &new);						\
	TEST_ASSERT(!memcmp(&new, &orig, sizeof(new)), "KVM modified sregs");	\
} while (0)

#define KVM_ALWAYS_ALLOWED_CR4 (X86_CR4_VME | X86_CR4_PVI | X86_CR4_TSD |	\
				X86_CR4_DE | X86_CR4_PSE | X86_CR4_PAE |	\
				X86_CR4_MCE | X86_CR4_PGE | X86_CR4_PCE |	\
				X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT)

static uint64_t calc_supported_cr4_feature_bits(void)
{
	uint64_t cr4 = KVM_ALWAYS_ALLOWED_CR4;

	if (kvm_cpu_has(X86_FEATURE_UMIP))
		cr4 |= X86_CR4_UMIP;
	if (kvm_cpu_has(X86_FEATURE_LA57))
		cr4 |= X86_CR4_LA57;
	if (kvm_cpu_has(X86_FEATURE_VMX))
		cr4 |= X86_CR4_VMXE;
	if (kvm_cpu_has(X86_FEATURE_SMX))
		cr4 |= X86_CR4_SMXE;
	if (kvm_cpu_has(X86_FEATURE_FSGSBASE))
		cr4 |= X86_CR4_FSGSBASE;
	if (kvm_cpu_has(X86_FEATURE_PCID))
		cr4 |= X86_CR4_PCIDE;
	if (kvm_cpu_has(X86_FEATURE_XSAVE))
		cr4 |= X86_CR4_OSXSAVE;
	if (kvm_cpu_has(X86_FEATURE_SMEP))
		cr4 |= X86_CR4_SMEP;
	if (kvm_cpu_has(X86_FEATURE_SMAP))
		cr4 |= X86_CR4_SMAP;
	if (kvm_cpu_has(X86_FEATURE_PKU))
		cr4 |= X86_CR4_PKE;

	return cr4;
}

static void test_cr_bits(struct kvm_vcpu *vcpu, uint64_t cr4)
{
	struct kvm_sregs sregs;
	int rc, i;

	vcpu_sregs_get(vcpu, &sregs);
	sregs.cr0 &= ~(X86_CR0_CD | X86_CR0_NW);
	sregs.cr4 |= cr4;
	rc = _vcpu_sregs_set(vcpu, &sregs);
	TEST_ASSERT(!rc, "Failed to set supported CR4 bits (0x%lx)", cr4);

	TEST_ASSERT(!!(sregs.cr4 & X86_CR4_OSXSAVE) ==
		    (vcpu->cpuid && vcpu_cpuid_has(vcpu, X86_FEATURE_OSXSAVE)),
		    "KVM didn't %s OSXSAVE in CPUID as expected",
		    (sregs.cr4 & X86_CR4_OSXSAVE) ? "set" : "clear");

	TEST_ASSERT(!!(sregs.cr4 & X86_CR4_PKE) ==
		    (vcpu->cpuid && vcpu_cpuid_has(vcpu, X86_FEATURE_OSPKE)),
		    "KVM didn't %s OSPKE in CPUID as expected",
		    (sregs.cr4 & X86_CR4_PKE) ? "set" : "clear");

	vcpu_sregs_get(vcpu, &sregs);
	TEST_ASSERT(sregs.cr4 == cr4, "sregs.CR4 (0x%llx) != CR4 (0x%lx)",
		    sregs.cr4, cr4);

	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_UMIP);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_LA57);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_VMXE);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_SMXE);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_FSGSBASE);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_PCIDE);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_OSXSAVE);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_SMEP);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_SMAP);
	TEST_INVALID_CR_BIT(vcpu, cr4, sregs, X86_CR4_PKE);

	for (i = 32; i < 64; i++)
		TEST_INVALID_CR_BIT(vcpu, cr0, sregs, BIT(i));

	/* NW without CD is illegal, as is PG without PE. */
	TEST_INVALID_CR_BIT(vcpu, cr0, sregs, X86_CR0_NW);
	TEST_INVALID_CR_BIT(vcpu, cr0, sregs, X86_CR0_PG);
}

int main(int argc, char *argv[])
{
	struct kvm_sregs sregs;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int rc;

	/*
	 * Create a dummy VM, specifically to avoid doing KVM_SET_CPUID2, and
	 * use it to verify KVM enforces guest CPUID even if *userspace* never
	 * sets CPUID.
	 */
	vm = vm_create_barebones();
	vcpu = __vm_vcpu_add(vm, 0);
	test_cr_bits(vcpu, KVM_ALWAYS_ALLOWED_CR4);
	kvm_vm_free(vm);

	/* Create a "real" VM with a fully populated guest CPUID and verify
	 * APIC_BASE and all supported CR4 can be set.
	 */
	vm = vm_create_with_one_vcpu(&vcpu, NULL);

	vcpu_sregs_get(vcpu, &sregs);
	sregs.apic_base = 1 << 10;
	rc = _vcpu_sregs_set(vcpu, &sregs);
	TEST_ASSERT(rc, "Set IA32_APIC_BASE to %llx (invalid)",
		    sregs.apic_base);
	sregs.apic_base = 1 << 11;
	rc = _vcpu_sregs_set(vcpu, &sregs);
	TEST_ASSERT(!rc, "Couldn't set IA32_APIC_BASE to %llx (valid)",
		    sregs.apic_base);

	test_cr_bits(vcpu, calc_supported_cr4_feature_bits());

	kvm_vm_free(vm);

	return 0;
}
