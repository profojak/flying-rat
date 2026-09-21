{
  description = "Procedural Maze Explorer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.clang
            pkgs.clang-tools
            pkgs.cmake
            pkgs.ninja
            pkgs.glfw
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
            pkgs.apple-sdk
          ];

          # Expose hidden Nix flags.
          shellHook = ''export CXXFLAGS="''${CXXFLAGS:-} ''${NIX_CFLAGS_COMPILE:-} $(sed 's/-cxx-isystem/-isystem/g' "$NIX_CC/nix-support/libcxx-cxxflags")"'';
        };
      });
}
