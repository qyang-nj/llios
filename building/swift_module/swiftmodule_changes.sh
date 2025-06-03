#!/bin/zsh

# This scripts demonstrates what source change will cause a change in the .swiftmodule file.

set -e

cat > lib.swift <<EOF
class Foo {
  init(privateLet: String) {
    self.privateLet = privateLet
  }

  private let privateLet: String

  private let privateLet2: String = "__private_Let2"

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

  public func publicFunc() -> String {
    return "Public function"
  }
}

// PublicClassPlaceholder
EOF

change_private_let_name() {
  message="Changing a private let name"
  sed -i '' 's/privateLet/newPrivateLet/g' lib.swift
}

change_private_let_value() {
  message="Changing a private let value"
  sed -i '' 's/__private_Let2/newValue/g' lib.swift
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
  change_private_let_name
  change_private_let_value
  change_private_var_value
  change_private_lazy_var
  change_private_computed_var
  change_private_func
  change_private_inline_func
  add_public_class
)

build_swiftmodule() {
    # The following flags have no effect on the .swiftmodule file for this example:
    #   -Xfrontend -disable-reflection-metadata
    #   -Xfrontend -no-serialize-debugging-options
    #   -enable-library-evolution
    extra_flags=()
    xcrun swiftc -emit-module -parse-as-library -module-name=lib -o lib.swiftmodule $extra_flags lib.swift
    checksum=$(sha256sum lib.swiftmodule | cut -d ' ' -f1)
}

# Initial compilation and hash calculation
build_swiftmodule
echo "Initial checksum: $checksum"
previous_checksum=$checksum

# Execute them in order
for change in "${changes[@]}"; do
  $change
  printf "%-42s" "$message:"

  build_swiftmodule
  current_checksum=$checksum

  printf "${current_checksum:0:16} "
  if [[ "$previous_checksum" == "$current_checksum" ]]; then
    echo " \033[0;32mUnchanged\033[0m"
  else
    echo " \033[0;31mChanged\033[0m"
  fi

  previous_checksum=$current_checksum
done
