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
            pkgs.glm
            pkgs.vulkan-headers
            pkgs.vulkan-loader
            pkgs.vulkan-validation-layers
            pkgs.shader-slang
            pkgs.spirv-tools
          ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [
            pkgs.apple-sdk
            pkgs.moltenvk
          ];

          shellHook = ''
            # Expose hidden Nix flags.
            export CXXFLAGS="''${CXXFLAGS:-} ''${NIX_CFLAGS_COMPILE:-} $(sed 's/-cxx-isystem/-isystem/g' "$NIX_CC/nix-support/libcxx-cxxflags")"

            # Find MoltenVK and validation layers on MacOS.
            if [ -d "${pkgs.moltenvk}/share/vulkan/icd.d" ]; then
              export VK_ICD_FILENAMES="${pkgs.moltenvk}/share/vulkan/icd.d/MoltenVK_icd.json"
            fi
            if [ -d "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d" ]; then
              export VK_LAYER_PATH="${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d"
            fi
          ''
          # Resolve GLFW's runtime Vulkan dependency.
          + pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
            export DYLD_LIBRARY_PATH="${pkgs.vulkan-loader}/lib''${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
          ''
          + pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isLinux ''
            export LD_LIBRARY_PATH="${pkgs.vulkan-loader}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
          '';
        };
      });
}
