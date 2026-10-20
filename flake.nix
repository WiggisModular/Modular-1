{
  description = "Modular-1 dev shell: web app + JieLi firmware toolchain";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux"; # the JieLi toolchain is Linux x86-64 only
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          (python3.withPackages (ps: [ ps.mido ps.python-rtmidi ps.pillow ps.fonttools ]))
          nodejs
          git
          curl
          xz
          gnumake
        ];

        # JieLi's prebuilt clang is a dynamically linked FHS binary. NixOS needs
        # programs.nix-ld.enable = true for it to run (it does not here otherwise).
        NIX_LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [ pkgs.stdenv.cc.cc.lib pkgs.zlib ];

        shellHook = ''
          export JIELI_TOOLCHAIN="''${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
          export AC79_SDK="''${AC79_SDK:-$HOME/.jieli/ac79-sdk}"
          if [ ! -x "$JIELI_TOOLCHAIN/pi32v2/bin/clang" ]; then
            echo "JieLi toolchain missing: run tools/get_toolchain.sh"
          fi
          if [ ! -d "$AC79_SDK" ]; then
            echo "AC79 SDK missing at $AC79_SDK (see CLAUDE.md, next steps 1)"
          fi
        '';
      };
    };
}
