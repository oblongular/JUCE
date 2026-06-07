{
  description = "JUCE development shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
    in {
      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [
              cmake
              ninja
              pkg-config
              clang
            ];

            buildInputs = with pkgs; [
              # juce_audio_devices
              alsa-lib
              alsa-utils
              jack2

              # juce_audio_processors
              ladspa-sdk

              # juce_core
              curl

              # juce_graphics
              freetype
              fontconfig

              # juce_gui_basics
              libx11
              libxcomposite
              libxcursor
              libxext
              libxinerama
              libxrandr
              libxrender

              # juce_gui_extra
              webkitgtk_4_1

              # juce_opengl
              libGLU
              mesa
            ];
          };
        });
    };
}
