#include <cassert>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "bridges/cpu_managed_stream.h"
#include "bridges/fpga_managed_stream.h"
#include "core/simif.h"

#include <fpga_mgmt.h>
#include <fpga_pci.h>

class simif_f1_t final : public simif_t, public BiDirectionalManagedStreamIO {
public:
  simif_f1_t(const TargetConfig &config, const std::vector<std::string> &args);
  ~simif_f1_t();

  void write(size_t addr, uint32_t data) override;
  uint32_t read(size_t addr) override;

  uint32_t is_write_ready();
  void check_rc(int rc, char *infostr);
  void fpga_shutdown();
  void fpga_setup(int slot_id, const std::string &agfi);

  CPUManagedStreamIO &get_cpu_managed_stream_io() override { return *this; }
  FPGAManagedStreamIO &get_fpga_managed_stream_io() override { return *this; }

private:
  uint32_t mmio_read(size_t addr) override { return read(addr); }
  void mmio_write(size_t addr, uint32_t value) override {
    return write(addr, value);
  }
  size_t
  cpu_managed_axi4_write(size_t addr, const char *data, size_t size) override;
  size_t cpu_managed_axi4_read(size_t addr, char *data, size_t size) override;
  uint64_t get_beat_bytes() const override {
    return config.cpu_managed->beat_bytes();
  }
  char *get_memory_base() override { return NULL; }

  int edma_write_fd;
  int edma_read_fd;
  pci_bar_handle_t pci_bar_handle;
  void *bar0_base;
  uint32_t bar0_size = 4*1024*1024; /* 4M */
};

simif_f1_t::simif_f1_t(const TargetConfig &config,
                       const std::vector<std::string> &args)
    : simif_t(config) {

  int slot_id = -1;
  std::string agfi;
  for (auto &arg : args) {
    if (arg.find("+slotid=") == 0) {
      slot_id = atoi((arg.c_str()) + 8);
      continue;
    }
    if (arg.find("+agfi=") == 0) {
      agfi += arg.c_str() + 6;
      if (agfi.find("agfi-") != 0 && agfi.size() != 22) {
        throw std::runtime_error("invalid AGFI: " + agfi);
      }
      continue;
    }
  }

  if (slot_id == -1) {
    fprintf(stderr, "Slot ID not specified. Assuming Slot 0\n");
    slot_id = 0;
  }

  fpga_setup(slot_id, agfi);
}

void simif_f1_t::check_rc(int rc, char *infostr) {
  if (rc) {
    if (infostr) {
      fprintf(stderr, "%s\n", infostr);
    }
    fprintf(stderr, "INVALID RETCODE: %d\n", rc, infostr);
    fpga_shutdown();
    exit(1);
  }
}

void simif_f1_t::fpga_shutdown() {
  if (bar0_base) {
    munmap(bar0_base, bar0_size);
    return;
  }
  int rc = fpga_pci_detach(pci_bar_handle);
  // don't call check_rc because of fpga_shutdown call. do it manually:
  if (rc) {
    fprintf(stderr, "Failure while detaching from the fpga: %d\n", rc);
  }
#if 0
  close(edma_write_fd);
  close(edma_read_fd);
#endif
}

/**
 * Amazon PCI Vendor ID.
 */
constexpr uint16_t pci_vendor_id = 0x1D0F;

/**
 * Amazon PCI Device ID pre-assigned by for F1 applications.
 */
constexpr uint16_t pci_device_id = 0xF010;

void simif_f1_t::fpga_setup(int slot_id, const std::string &agfi) {
  int rc = fpga_mgmt_init();
  check_rc(rc, "fpga_mgmt_init FAILED");

  // If an AGFI was specified, re-load the image.
  if (!agfi.empty()) {
    fprintf(stderr, "Flashing AGFI: %s\n", agfi.c_str());

    // Clear the existing image. Wait up to 10 seconds.
    rc = fpga_mgmt_clear_local_image_sync(slot_id, 10, 1000, nullptr);
    check_rc(rc, "Cannot clear image");

    // Load the image.
    std::unique_ptr<char[]> data(new char[agfi.size() + 1]);
    memcpy(data.get(), agfi.c_str(), agfi.size() + 1);
    rc = fpga_mgmt_load_local_image(slot_id, data.get());
    check_rc(rc, "Cannot load AGFI");

    // Wait and poll as long as the slot is busy.
    int status;
    do {
      sleep(1);

      struct fpga_mgmt_image_info info = {0};
      rc = fpga_mgmt_describe_local_image(slot_id, &info, 0);
      check_rc(rc, "Unable to get AFI information from slot.");
      status = info.status;
    } while (status == FPGA_STATUS_BUSY);
  }

  /* check AFI status */
  struct fpga_mgmt_image_info info = {0};

  /* get local image description, contains status, vendor id, and device id. */
  rc = fpga_mgmt_describe_local_image(slot_id, &info, 0);
  if (rc) {
    goto pcie_uio;
  }
  check_rc(rc,
           "Unable to get AFI information from slot. Are you running as root?");

  /* check to see if the slot is ready */
  if (info.status != FPGA_STATUS_LOADED) {
    rc = 1;
    check_rc(rc, "AFI in Slot is not in READY state !");
  }

  fprintf(stderr,
          "AFI ID for Slot %2u: %s\n",
          slot_id,
          (!info.ids.afi_id[0]) ? "none" : info.ids.afi_id);

  fprintf(stderr,
          "AFI PCI  Vendor ID: 0x%x, Device ID 0x%x\n",
          info.spec.map[FPGA_APP_PF].vendor_id,
          info.spec.map[FPGA_APP_PF].device_id);

  /* confirm that the AFI that we expect is in fact loaded */
  if (info.spec.map[FPGA_APP_PF].vendor_id != pci_vendor_id ||
      info.spec.map[FPGA_APP_PF].device_id != pci_device_id) {
    fprintf(
        stderr,
        "AFI does not show expected PCI vendor id and device ID. If the AFI "
        "was just loaded, it might need a rescan. Rescanning now.\n");

    rc = fpga_pci_rescan_slot_app_pfs(slot_id);
    check_rc(rc, "Unable to update PF for slot");
    /* get local image description, contains status, vendor id, and device id.
     */
    rc = fpga_mgmt_describe_local_image(slot_id, &info, 0);
    check_rc(rc, "Unable to get AFI information from slot");

    fprintf(stderr,
            "AFI ID for Slot %2u: %s\n",
            slot_id,
            (!info.ids.afi_id[0]) ? "none" : info.ids.afi_id);

    fprintf(stderr,
            "AFI PCI  Vendor ID: 0x%x, Device ID 0x%x\n",
            info.spec.map[FPGA_APP_PF].vendor_id,
            info.spec.map[FPGA_APP_PF].device_id);

    /* confirm that the AFI that we expect is in fact loaded after rescan */
    if (info.spec.map[FPGA_APP_PF].vendor_id != pci_vendor_id ||
        info.spec.map[FPGA_APP_PF].device_id != pci_device_id) {
      rc = 1;
      check_rc(rc,
               "The PCI vendor id and device of the loaded AFI are not "
               "the expected values.");
    }
  }

  /* attach to BAR0 */
  pci_bar_handle = PCI_BAR_HANDLE_INIT;
  rc = fpga_pci_attach(slot_id, FPGA_APP_PF, APP_PF_BAR0, 0, &pci_bar_handle);
  check_rc(rc, "fpga_pci_attach FAILED");

#if 0
  // EDMA setup
  char device_file_name[256];
  char device_file_name2[256];

  sprintf(device_file_name, "/dev/xdma%d_h2c_0", slot_id);
  printf("Using xdma write queue: %s\n", device_file_name);
  sprintf(device_file_name2, "/dev/xdma%d_c2h_0", slot_id);
  printf("Using xdma read queue: %s\n", device_file_name2);


  edma_write_fd = open(device_file_name, O_WRONLY);
  edma_read_fd = open(device_file_name2, O_RDONLY);
  assert(edma_write_fd >= 0);
  assert(edma_read_fd >= 0);
#endif
  if (0) {
pcie_uio:
    int fd = open("/dev/uio0", O_RDWR | O_SYNC);
    assert(fd != -1);

    bar0_base = mmap(0, bar0_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(bar0_base != MAP_FAILED);

    close(fd);

    volatile uint32_t *reg_ptr = (uint32_t *)(bar0_base + 1*1024*1024);
    uint32_t value = *reg_ptr;
    fprintf(stderr, "Reset firesim, value=%x\n", value);
    assert(value != 0xFFFFFFFF);
    if (value == 1) {
      *reg_ptr = 0;
      usleep(100);
      *reg_ptr = 1;
      usleep(100);
    }
    /* map axi 0-16G to host 0x4_0000_0000
     * reserved 16G mem in host
     *  GRUB_CMDLINE_LINUX_DEFAULT="text pci=noaer memmap=16G$0x400000000 intel_iommu=off"
     */
    uint32_t xdma_ofst = 3 * 1024 * 1024;
    reg_ptr = (uint32_t *)(bar0_base + xdma_ofst + 0x208); *reg_ptr = 0x00000004; // AXIBAR2PCIEBAR0_U
    reg_ptr = (uint32_t *)(bar0_base + xdma_ofst + 0x20C); *reg_ptr = 0x00000000; // AXIBAR2PCIEBAR0_L
    fprintf(stderr, "map: DRAM addr 0 -> host %08x,%08x\n",
        *(uint32_t *)(bar0_base + xdma_ofst + 0x208),
        *(uint32_t *)(bar0_base + xdma_ofst + 0x20C));
    return;
  }
}

simif_f1_t::~simif_f1_t() { fpga_shutdown(); }

void simif_f1_t::write(size_t addr, uint32_t data) {
  if (bar0_base) {
    volatile uint32_t *reg_ptr = (uint32_t *)(bar0_base + addr);
    *reg_ptr = data;
    return;
  }
  int rc = fpga_pci_poke(pci_bar_handle, addr, data);
  check_rc(rc, NULL);
  //printf("w: %x, %x\n", addr, data);
}

uint32_t simif_f1_t::read(size_t addr) {
  uint32_t value;
  if (bar0_base) {
    volatile uint32_t *reg_ptr = (uint32_t *)(bar0_base + addr);
    value = *reg_ptr;
    return value & 0xFFFFFFFF;
  }
  int rc = fpga_pci_peek(pci_bar_handle, addr, &value);
  //printf("r: %x, %x\n", addr, value);
  return value & 0xFFFFFFFF;
}

size_t simif_f1_t::cpu_managed_axi4_read(size_t addr, char *data, size_t size) {
  return /*::pread(edma_read_fd, data, size, addr)*/0;
}

size_t
simif_f1_t::cpu_managed_axi4_write(size_t addr, const char *data, size_t size) {
  return /*::pwrite(edma_write_fd, data, size, addr)*/0;
}

uint32_t simif_f1_t::is_write_ready() {
  if (bar0_base) {
    volatile uint32_t *reg_ptr = (uint32_t *)(bar0_base + 0x4);
    uint32_t value = *reg_ptr;
    return value & 0xFFFFFFFF;
  }
  uint64_t addr = 0x4;
  uint32_t value;
  int rc = fpga_pci_peek(pci_bar_handle, addr, &value);
  check_rc(rc, NULL);
  return value & 0xFFFFFFFF;
}

std::unique_ptr<simif_t>
create_simif(const TargetConfig &config, int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return std::make_unique<simif_f1_t>(config, args);
}
