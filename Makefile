# Top-level Makefile for advanced-linux labs
# Orchestrates building and running demos across all directories

.PHONY: all clean run help $(C_LABS) $(RUST_LABS) build-rust docs-rust clean-rust

# C/C++ labs
C_LABS := 01-virtio 02-hypercalls 03-uffd 04-io_uring 07-ioeventfd 08-irqfd 09-kick-roundtrip 11-scm-rights

# Rust labs (using cargo)
RUST_LABS := 05-lockfree 06-cgroups

help:
	@echo "Advanced Linux Labs - Top-level Makefile"
	@echo ""
	@echo "Available targets:"
	@echo "  make all       - Build all C/C++ and Rust labs (+ Rust docs)"
	@echo "  make docs-rust - Regenerate committed rustdoc HTML in <lab>/docs/"
	@echo "  make clean     - Clean all built artifacts"
	@echo "  make run       - Run the default lab (03-uffd)"
	@echo "  make run-LABS  - Run a specific lab (e.g., make run-01-virtio)"
	@echo ""
	@echo "C/C++ labs:"
	@for lab in $(C_LABS); do echo "  - $$lab"; done
	@echo ""
	@echo "Rust labs:"
	@for lab in $(RUST_LABS); do echo "  - $$lab"; done

all: $(C_LABS) build-rust docs-rust

$(C_LABS):
	$(MAKE) -C $@

build-rust:
	@for lab in $(RUST_LABS); do \
		echo "Building $$lab..."; \
		(cd $$lab && cargo build --release) || true; \
	done

# rustdoc output lands in target/doc (gitignored); copy it to <lab>/docs so it
# is committed, plus an index.html that redirects to the crate's page (crate
# name = Cargo package name with '-' -> '_'). Made reproducible so `make all`
# leaves git clean: target/doc is wiped first (cargo never deletes stale
# content-hashed search-index files), and help/settings.html get a fixed
# data-current-crate (rustdoc records whichever crate it happened to finish last).
docs-rust:
	@set -e; for lab in $(RUST_LABS); do \
		echo "Documenting $$lab..."; \
		(cd $$lab && rm -rf target/doc && cargo doc --no-deps --quiet); \
		crate=$$(sed -n 's/^name *= *"\(.*\)"/\1/p' $$lab/Cargo.toml | head -1 | tr - _); \
		rm -rf $$lab/docs && cp -r $$lab/target/doc $$lab/docs && rm -f $$lab/docs/.lock; \
		sed -i "s/data-current-crate=\"[^\"]*\"/data-current-crate=\"$$crate\"/" $$lab/docs/help.html $$lab/docs/settings.html; \
		printf '<!DOCTYPE html>\n<meta charset="utf-8">\n<title>%s docs</title>\n<meta http-equiv="refresh" content="0; url=%s/index.html">\n<a href="%s/index.html">%s crate docs</a>\n' \
			"$$crate" "$$crate" "$$crate" "$$crate" > $$lab/docs/index.html; \
	done

clean: clean-rust
	@for lab in $(C_LABS); do $(MAKE) -C $$lab clean; done
	@echo "All labs cleaned."

clean-rust:
	@for lab in $(RUST_LABS); do \
		(cd $$lab && cargo clean) || true; \
	done

# Run a specific lab: make run-03-uffd
run-%:
	$(MAKE) -C $* run

# Default run target: 03-uffd (most interesting demo)
run: run-03-uffd

.PHONY: $(C_LABS) run-%
