#!/bin/zsh

# This scripts demonstrates what source change will cause a change in the .swiftmodule file.

set -e

cat >lib.swift <<EOF
class Foo {
  init(privateLet: String) {
    self.privateInitLet = privateLet
  }

  private let privateInitLet: String

  private let privateLet2: String = "__privateLet2"

  private var privateVar = "__privateVar"

  lazy private var privateLazyVar = {
    return "__privateLazyVar"
  }()

  private var privateComputedVar: String {
    return "__privateComputedVar"
  }

  private func privateFunc() -> String {
    return "__privateFunc"
  }

  @inline(__always) func privateInlineFunc() -> String {
    return "__privateInlineFunc"
  }
}

// PublicClassPlaceholder
EOF

change_private_let_name() {
  message="Changing a private let name"
  sed -i '' 's/privateInitLet/newPrivateInitLet/g' lib.swift
}

change_private_let_value() {
  message="Changing a private let value"
  sed -i '' 's/__privateLet2/newValue/g' lib.swift
}

change_private_var_value() {
  message="Changing a private var value"
  sed -i '' 's/__privateVar/newValue/g' lib.swift
}

change_private_lazy_var() {
  message="Changing a private lazy var"
  sed -i '' 's/__privateLazyVar/newValue/g' lib.swift
}

change_private_computed_var() {
  message="Changing a private computed var"
  sed -i '' 's/__privateComputedVar/newValue/g' lib.swift
}

change_private_func() {
  message="Changing a private func body"
  sed -i '' 's/__privateFunc/newValue/g' lib.swift
}

change_private_inline_func() {
  message="Changing a private inline func body"
  sed -i '' 's/__privateInlineFunc/newValue/g' lib.swift
}

add_public_class() {
  # This change should make .swiftmodule change
  message="Adding a public class"
  sed -i '' 's/\/\/ PublicClassPlaceholder/public class Foo2 {}/g' lib.swift
}

changes=(
  add_public_class
  change_private_let_name
  change_private_let_value
  change_private_var_value
  change_private_lazy_var
  change_private_computed_var
  change_private_func
  change_private_inline_func
)

build_swiftmodule() {
  # The following flags have no effect on the .swiftmodule file for this example:
  #   -Xfrontend -disable-reflection-metadata
  #   -Xfrontend -no-serialize-debugging-options
  extra_flags=()
  xcrun swiftc -enable-library-evolution -emit-module -emit-module-interface -parse-as-library -module-name=lib -o lib.swiftmodule $extra_flags lib.swift
  swiftmodule_checksum=$(sha256sum lib.swiftmodule | cut -d ' ' -f1)
  swiftinterface_checksum=$(sha256sum lib.swiftinterface | cut -d ' ' -f1)
}

# Initial compilation and hash calculation
build_swiftmodule
printf "%-42s.swiftmodule (${swiftmodule_checksum:0:16})       \t.swiftinterface (${swiftinterface_checksum:0:16})\n" "Initial:"
previous_swiftmodule_checksum=$swiftmodule_checksum
previous_swiftinterface_checksum=$swiftinterface_checksum

readonly changed="\033[0;31mChanged\033[0m"
readonly unchanged="\033[0;32mUnchanged\033[0m"

# Execute them in order
for change in "${changes[@]}"; do
  $change
  printf "%-42s" "$message:"

  build_swiftmodule

  [[ "$previous_swiftmodule_checksum" == "$swiftmodule_checksum" ]] && stat=$unchanged || stat=$changed
  echo -n ".swiftmodule (${swiftmodule_checksum:0:16} $stat)"

  [[ "$previous_swiftinterface_checksum" == "$swiftinterface_checksum" ]] && stat=$unchanged || stat=$changed
  echo "\t.swiftinterface (${swiftinterface_checksum:0:16} $stat)"

  previous_swiftmodule_checksum=$swiftmodule_checksum
  previous_swiftinterface_checksum=$swiftinterface_checksum
done
