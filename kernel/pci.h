#ifndef FOS_PCI_H
#define FOS_PCI_H

#include "types.h"

struct pci_device {
    u8  bus, slot, func;
    u16 vendor, device;
    u8  class_code, subclass, prog_if, revision;
};

typedef void (*pci_enum_fn)(struct pci_device *);

void pci_scan(pci_enum_fn fn);
u32  pci_read_config(struct pci_device *d, u8 off, u8 len);

#endif