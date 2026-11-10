{
  description = "IFCFilter: prepares IFC models for federation";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      # IfcOpenShell's parser only: no geometry engine, and just the schemas the models use
      ifcparse = pkgs.stdenv.mkDerivation {
        pname = "ifcopenshell-parse";
        version = "0.9.0";
        src = pkgs.fetchFromGitHub {
          owner = "IfcOpenShell";
          repo = "IfcOpenShell";
          rev = "ifcopenshell-v0.9.0";
          hash = "sha256-Sg4FBKJvjOZxG83ugBFPNkHPfwLpSOeJQpRLMZqbr9Q=";
        };
        cmakeDir = "../cmake";
        nativeBuildInputs = [ pkgs.cmake ];
        buildInputs = [ pkgs.boost ];
        cmakeFlags = [
          "-DSCHEMA_VERSIONS=2x3;4;4x3_add2"
          "-DBUILD_IFCGEOM=OFF"
          "-DBUILD_IFCPYTHON=OFF"
          "-DBUILD_CONVERT=OFF"
          "-DBUILD_GEOMSERVER=OFF"
          "-DBUILD_EXAMPLES=OFF"
          "-DWITH_OPENCASCADE=OFF"
          "-DWITH_CGAL=OFF"
          "-DCOLLADA_SUPPORT=OFF"
          "-DHDF5_SUPPORT=OFF"
        ];
      };
      ifcfilter = pkgs.stdenv.mkDerivation {
        pname = "ifcfilter";
        version = "0.1.0";
        src = ./.;
        nativeBuildInputs = [ pkgs.cmake ];
        buildInputs = [ ifcparse pkgs.boost pkgs.eigen pkgs.zlib pkgs.openmesh ];
      };
    in {
      packages.x86_64-linux = { inherit ifcparse ifcfilter; default = ifcfilter; };
      devShells.x86_64-linux.default = pkgs.mkShell {
        nativeBuildInputs = [ pkgs.cmake pkgs.gdb ];
        buildInputs = [ ifcparse pkgs.boost pkgs.eigen pkgs.zlib pkgs.openmesh ];
      };
    };
}
