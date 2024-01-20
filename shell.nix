# see https://ayats.org/blog/nix-cross/
let
  pkgs  = import <nixpkgs> {};
  pkgs' = pkgs.pkgsCross.riscv64-embedded;
in
  pkgs'.mkShell {
    depsBuildBuild = with pkgs; [
      gcc
      qemu
    ];

    nativeBuildInputs = with pkgs'.buildPackages; [
      gcc
    ];

    buildInputs = with pkgs'; [
      newlib
    ];

    shellHook = ''
      # 'make qemu' expects riscv64-unknown-elf-, but it's different w/ nix
      export TOOLPREFIX="riscv64-none-elf-"

      # make customize the PS1 to make it clear we're in a shell for the xv6-labs
      export PS1="\n\[\033[1;32m\][xv6-labs-shell:\w]\$\[\033[0m\] "
    '';
  }
