import SwiftUI

@available(iOS 13.0, *)
struct SwiftUIView: View {
  var body: some View {
    VStack {
      Text("Hello, World!").font(.title)
      Text("🎉🎉🎉").font(.subheadline)
    }
  }
}

@available(iOS 13.0, *)
struct SwiftUIView_Previews: PreviewProvider {
  static var previews: some View {
    SwiftUIView()
  }
}
