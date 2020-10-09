#include "logging.hpp"
#include <LIEF/ELF.hpp>
#include <QBDL/Engine.hpp>
#include <QBDL/arch.hpp>
#include <QBDL/loaders/ELF.hpp>
#include <QBDL/utils.hpp>

using namespace LIEF::ELF;

namespace QBDL::Loaders {

// This function is called by the _dl_resolve_internal()
//
// On x86-64 the plt/got push the **index** of the called function on
// the stack while on AArch64 it loads the relocation's address.
// Therefore, for this architecture we need to process the hint variable
// to convert it into an index
uintptr_t ELF::dl_resolve(void *loader, uintptr_t hint) {
  static constexpr size_t GOT_RESERVED_ENTRIES_SIZE = 3;
  auto &ldr = *reinterpret_cast<QBDL::Loaders::ELF *>(loader);
  const Binary &bin = ldr.get_binary();

  const ARCH arch = ldr.get_binary().header().machine_type();
  uintptr_t plt_sym_idx = hint;
  if (arch == ARCH::EM_AARCH64) {
    const uintptr_t got_base = bin.get(DYNAMIC_TAGS::DT_PLTGOT).value();
    plt_sym_idx =
        (plt_sym_idx - ldr.base_address_ - got_base) / sizeof(uintptr_t);
    // We need to remove the first reserved entries to get the index
    plt_sym_idx -= GOT_RESERVED_ENTRIES_SIZE;
  }

  auto pltgot = ldr.get_binary().pltgot_relocations();

  if (plt_sym_idx >= pltgot.size()) {
    QBDL::Logger::err("PLT index out of range: {:d}", plt_sym_idx);
    return 0;
  }

  const Relocation &plt_reloc = pltgot[plt_sym_idx];
  const Symbol &sym = plt_reloc.symbol();
  const uintptr_t sym_addr = ldr.engine_->symlink(ldr, sym);
  const uintptr_t addr_target = ldr.get_address(plt_reloc.address());

  QBDL_INFO("Address of {}: 0x{:x}", sym.name(), sym_addr);
  ldr.engine_->mem().write_ptr(ldr.arch(), addr_target, sym_addr);
  return sym_addr;
}

std::unique_ptr<ELF> ELF::from_file(const char *path, TargetSystem &engines,
                                    BIND binding) {
  Logger::info("Loading {}", path);
  if (not is_elf(path)) {
    Logger::err("{} is not an ELF file", path);
    return {};
  }
  std::unique_ptr<Binary> bin = Parser::parse(path);
  if (bin == nullptr) {
    Logger::err("Can't parse {}", path);
    return {};
  }
  return from_binary(std::move(bin), engines, binding);
}

std::unique_ptr<ELF> ELF::from_binary(std::unique_ptr<Binary> bin,
                                      TargetSystem &engines, BIND binding) {
  if (!engines.supports(*bin)) {
    return {};
  }
  std::unique_ptr<ELF> loader(new ELF{std::move(bin), engines});
  loader->load(binding);
  return loader;
}

ELF::ELF(std::unique_ptr<Binary> bin, TargetSystem &engines)
    : Loader::Loader(engines), bin_{std::move(bin)} {

  // Fill the symbol cache
  for (Symbol &sym : get_binary().dynamic_symbols()) {
    if (sym.value() > 0) {
      sym_exp_[sym.name()] = &sym;
    }
  }
}

uint64_t ELF::get_address(const std::string &sym) const {
  const Binary &binary = get_binary();
  const LIEF::Symbol *symbol = nullptr;
  if (binary.has_symbol(sym)) {
    symbol = &binary.get_symbol(sym);
  }
  if (symbol == nullptr) {
    return 0;
  }
  return base_address_ + get_rva(binary, symbol->value());
}

uint64_t ELF::get_address(uint64_t offset) const {
  return base_address_ + offset;
}

uint64_t ELF::entrypoint() const {
  const Binary &binary = get_binary();
  return base_address_ + (binary.entrypoint() - binary.imagebase());
}

void ELF::load(BIND binding) {
  Logger::info("this: 0x{:x}", reinterpret_cast<uintptr_t>(this));
  Binary &binary = get_binary();

  uint64_t virtual_size = binary.virtual_size();
  virtual_size -= binary.imagebase();
  virtual_size = page_align(virtual_size);

  Logger::debug("Virtual size: 0x{:x}", virtual_size);

  const uint64_t base_address_hint =
      engine_->base_address_hint(binary.imagebase(), virtual_size);
  const uint64_t base_address =
      engine_->mem().mmap(base_address_hint, virtual_size);
  if (base_address == 0) {
    Logger::err("mmap() failed! Abort.");
    return;
  }
  base_address_ = base_address;

  // Map segments
  // =======================================================
  for (const Segment &segment : binary.segments()) {
    if (segment.type() != SEGMENT_TYPES::PT_LOAD) {
      continue;
    }
    const uint64_t rva = get_rva(binary, segment.virtual_address());

    Logger::debug("Mapping {} - 0x{:x}", to_string(segment.type()), rva);
    const std::vector<uint8_t> &content = segment.content();
    if (content.size() > 0) {
      engine_->mem().write(base_address + rva, content.data(), content.size());
    }
  }

  using relocation_fcn_t = decltype(&ELF::reloc_x86_64);
  relocation_fcn_t relocator = nullptr;
  const LIEF::ELF::ARCH arch = get_binary().header().machine_type();
  switch (arch) {
  case LIEF::ELF::ARCH::EM_AARCH64: {
    relocator = &ELF::reloc_aarch64;
    break;
  }

  case LIEF::ELF::ARCH::EM_X86_64: {
    relocator = &ELF::reloc_x86_64;
    break;
  }

  default: {
    Logger::err("Relocations not supported for the architecture: {}",
                LIEF::ELF::to_string(arch));
    break;
  }
  }

  if (relocator == nullptr) {
    return;
  }

  // Perform relocations
  // =======================================================
  for (const Relocation &reloc : binary.dynamic_relocations()) {
    (*this.*relocator)(reloc, true);
  }

  // Bind symbols
  switch (binding) {
  case BIND::NOW: {
    bind_now(relocator);
    break;
  }

  case BIND::LAZY:
  case BIND::DEFAULT: {
    bind_lazy(relocator);
    break;
  }

  case BIND::NOT_BIND:
  default: {
    break;
  }
  }
}

void ELF::bind_lazy(ELF::relocator_t relocator) {
  // The beginning of the .got.plt is identified by the dynamic entry: DT_PLTGOT
  // or the symbol _GLOBAL_OFFSET_TABLE_. I would say that the DT_PLTGOT is more
  // reliable ...
  // The first three entries contain the following values:
  //
  // GOT[0] := Address of the PT_DYNAMIC segment
  // GOT[1] := Shared object identifier (point to the "link_map")
  // GOT[2] := Address to the lazy runtime resolver (_dl_runtime_resolve_XXX)
  //           which is defined in sysdeps/<arch>/dl-trampoline.S
  //
  // We can use this layout to setup our own _dl_runtime_resolve (GOT[2])
  // and using GOT[1] as a 'scratch variable' to pass a reference to the current
  // loader object (this)
  //
  // On Android lazy-binding is not "supported" in the sense that the Android
  // ELF binaries have all the information to bind them lazily but they didn't
  // want to support it in the Android loader (likely for security reasons)

  const Binary &bin = get_binary();
  const Arch binarch = arch();

  if (not bin.has(DYNAMIC_TAGS::DT_PLTGOT)) {
    Logger::warn("Missing DT_PLTGOT. Can't lazy-bind this binary");
    return;
  }

  const uintptr_t got_addr =
      get_address(bin.get(DYNAMIC_TAGS::DT_PLTGOT).value());
  engine_->mem().write_ptr(binarch, got_addr + 1 * sizeof(uintptr_t),
                           reinterpret_cast<uintptr_t>(this));

  engine_->mem().write_ptr(binarch, got_addr + 2 * sizeof(uintptr_t),
                           reinterpret_cast<uintptr_t>(&_dl_resolve_internal));

  for (const Relocation &reloc : bin.pltgot_relocations()) {
    (*this.*relocator)(reloc, true);
  }
}

void ELF::bind_now(ELF::relocator_t relocator) {
  for (const Relocation &reloc : get_binary().pltgot_relocations()) {
    (*this.*relocator)(reloc, false);
  }
}

uintptr_t ELF::resolve(const LIEF::ELF::Symbol &sym) {
  // Check if the given symbol is not exported by the binary itself.
  // This could append in the case of a static link
  // where the linker produces all the plt/got mechanism
  // even though the symbol is in the final binary
  const auto it_sym = sym_exp_.find(sym.name());
  if (it_sym == std::end(sym_exp_)) {
    return 0;
  }
  return get_address(it_sym->second->value());
}

void ELF::reloc_x86_64(const LIEF::ELF::Relocation &reloc, bool is_lazy) {
  const Arch binarch = arch();
  const auto type = static_cast<RELOC_x86_64>(reloc.type());
  const uintptr_t addr_target = base_address_ + reloc.address();
  switch (type) {
  case RELOC_x86_64::R_X86_64_RELATIVE: {
    engine_->mem().write_ptr(binarch, addr_target,
                             base_address_ + reloc.addend());
    break;
  }

  case RELOC_x86_64::R_X86_64_JUMP_SLOT: {

    // First check if the symbol is not exported by the binary itself:
    if (uintptr_t addr = resolve(reloc.symbol()); addr != 0) {
      engine_->mem().write_ptr(binarch, addr_target, addr + reloc.addend());
      break;
    }

    if (is_lazy) {
      const uintptr_t value = engine_->mem().read_ptr(binarch, addr_target);
      engine_->mem().write_ptr(binarch, addr_target, base_address_ + value);
    } else {
      const uintptr_t sym_addr = engine_->symlink(*this, reloc.symbol());
      engine_->mem().write_ptr(binarch, addr_target, sym_addr + reloc.addend());
    }
    break;
  }
  case RELOC_x86_64::R_X86_64_GLOB_DAT: {
    // First check if the symbol is not exported by the binary itself:
    if (uintptr_t addr = resolve(reloc.symbol()); addr != 0) {
      engine_->mem().write_ptr(binarch, addr_target, addr + reloc.addend());
      break;
    }

    const uintptr_t addr = engine_->symlink(*this, reloc.symbol());
    engine_->mem().write_ptr(binarch, addr_target, addr + reloc.addend());
    break;
  }

  case RELOC_x86_64::R_X86_64_COPY: {
    const uintptr_t sym_addr = engine_->symlink(*this, reloc.symbol());
    engine_->mem().write(addr_target, reinterpret_cast<const void *>(sym_addr),
                         reloc.symbol().size());
    break;
  }

  default: {
    Logger::warn("Relocation type '{}' is not supported!", to_string(type));
  }
  }
}

Arch ELF::arch() const { return Arch::from_bin(get_binary()); }

void ELF::reloc_aarch64(const LIEF::ELF::Relocation &reloc, bool is_lazy) {
  const Arch binarch = arch();
  const auto type = static_cast<RELOC_AARCH64>(reloc.type());
  const uintptr_t addr_target = base_address_ + reloc.address();
  switch (type) {
  case RELOC_AARCH64::R_AARCH64_RELATIVE: {
    engine_->mem().write_ptr(binarch, addr_target,
                             base_address_ + reloc.addend());
    break;
  }

  case RELOC_AARCH64::R_AARCH64_JUMP_SLOT: {
    // First check if the symbol is not exported by the binary itself:
    if (uintptr_t addr = resolve(reloc.symbol()); addr != 0) {
      engine_->mem().write_ptr(binarch, addr_target, addr + reloc.addend());
      break;
    }
    if (is_lazy) {
      const uintptr_t value = engine_->mem().read_ptr(binarch, addr_target);
      engine_->mem().write_ptr(binarch, addr_target, base_address_ + value);
    } else {
      const uintptr_t sym_addr = engine_->symlink(*this, reloc.symbol());
      engine_->mem().write_ptr(binarch, addr_target, sym_addr + reloc.addend());
    }
    break;
  }
  case RELOC_AARCH64::R_AARCH64_GLOB_DAT: {
    // First check if the symbol is not exported by the binary itself:
    if (uintptr_t addr = resolve(reloc.symbol()); addr != 0) {
      engine_->mem().write_ptr(binarch, addr_target, addr + reloc.addend());
      break;
    }
    const uintptr_t sym_addr = engine_->symlink(*this, reloc.symbol());
    engine_->mem().write_ptr(binarch, addr_target, sym_addr + reloc.addend());
    break;
  }

  case RELOC_AARCH64::R_AARCH64_COPY: {
    const uintptr_t sym_addr = engine_->symlink(*this, reloc.symbol());
    engine_->mem().write(addr_target, reinterpret_cast<const void *>(sym_addr),
                         reloc.symbol().size());
    break;
  }

  default: {
    Logger::warn("Relocation type '{}' is not supported!", to_string(type));
  }
  }
}

uint64_t ELF::get_rva(const Binary &bin, uint64_t addr) const {
  if (addr >= bin.imagebase()) {
    return addr - bin.imagebase();
  }
  return addr;
}

ELF::~ELF() = default;

} // namespace QBDL::Loaders
