{
  description = "m701 - a dead simple music player";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, utils }:
    utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "m701";
          version = "0.1.0";

          src = ./.;

          buildInputs = [
            pkgs.ncurses
            pkgs.alsa-lib
          ];

          makeFlags = [ "BINDIR=$(out)/bin" ];

          preInstall = ''
            mkdir -p $out/bin
          '';
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.default ];
          
          nativeBuildInputs = [
            pkgs.gnumake
            pkgs.gdb
          ];
        };
      });
}
