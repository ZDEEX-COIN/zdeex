package=rust
$(package)_version=1.32.0
$(package)_download_path=https://static.rust-lang.org/dist
$(package)_file_name_linux=rust-$($(package)_version)-x86_64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_linux=e024698320d76b74daf0e6e71be3681a1e7923122e3ebd03673fcac3ecc23810
$(package)_file_name_darwin=rust-$($(package)_version)-x86_64-apple-darwin.tar.gz
$(package)_sha256_hash_darwin=f0dfba507192f9b5c330b5984ba71d57d434475f3d62bd44a39201e36fa76304
$(package)_file_name_mingw32=rust-$($(package)_version)-x86_64-pc-windows-gnu.tar.gz
$(package)_sha256_hash_mingw32=358e1435347c67dbf33aa9cad6fe501a833d6633ed5d5aa1863d5dffa0349be9
$(package)_file_name_aarch64_linux=rust-$($(package)_version)-aarch64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_aarch64_linux=60def40961728212da4b3a9767d5a2ddb748400e150a5f8a6d5aa0e1b8ba1cee

# Mapping from GCC canonical hosts to Rust targets
# If a mapping is not present, we assume they are identical, unless $host_os is
# "darwin", in which case we assume x86_64-apple-darwin.
$(package)_rust_target_x86_64-w64-mingw32=x86_64-pc-windows-gnu

# Mapping from Rust targets to SHA-256 hashes
$(package)_rust_std_sha256_hash_aarch64-unknown-linux-gnu=346efe3aef2aff7b71a611bf7661bcec5f9bc4025a599c2866ec5fd330247cb9
$(package)_rust_std_sha256_hash_x86_64-apple-darwin=b736d035a97f830585360e54e3f8877b68c942211cf0a75e805f34bfb36103a6
$(package)_rust_std_sha256_hash_x86_64-pc-windows-gnu=cad5f1454d591c13eeb3657f1c9dbfeb30e648f59680bd0765b94c63e7afc49e

define rust_target
$(if $($(1)_rust_target_$(2)),$($(1)_rust_target_$(2)),$(if $(findstring darwin,$(3)),x86_64-apple-darwin,$(2)))
endef

ifneq ($(canonical_host),$(build))
$(package)_rust_target=$(call rust_target,$(package),$(canonical_host),$(host_os))
$(package)_exact_file_name=rust-std-$($(package)_version)-$($(package)_rust_target).tar.gz
$(package)_exact_sha256_hash=$($(package)_rust_std_sha256_hash_$($(package)_rust_target))
$(package)_build_subdir=buildos
$(package)_extra_sources=$($(package)_file_name_$(build_os))

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_download_file),$($(package)_file_name),$($(package)_sha256_hash)) && \
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name_$(build_os)),$($(package)_file_name_$(build_os)),$($(package)_sha256_hash_$(build_os)))
endef

define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_sha256_hash)  $($(package)_source)" > $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  echo "$($(package)_sha256_hash_$(build_os))  $($(package)_source_dir)/$($(package)_file_name_$(build_os))" >> $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  mkdir $(canonical_host) && \
  tar --strip-components=1 -xf $($(package)_source) -C $(canonical_host) && \
  mkdir buildos && \
  tar --strip-components=1 -xf $($(package)_source_dir)/$($(package)_file_name_$(build_os)) -C buildos
endef

define $(package)_stage_cmds
  bash ./install.sh --destdir=$($(package)_staging_dir) --prefix=$(host_prefix)/native --disable-ldconfig --without=rust-docs && \
  ../$(canonical_host)/install.sh --destdir=$($(package)_staging_dir) --prefix=$(host_prefix)/native --disable-ldconfig --without=rust-docs
endef
else

define $(package)_stage_cmds
  bash ./install.sh --destdir=$($(package)_staging_dir) --prefix=$(host_prefix)/native --disable-ldconfig --without=rust-docs
endef
endif
