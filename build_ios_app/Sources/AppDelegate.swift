// Created by Qing Yang on 3/11/20.
// Copyright © 2020 Airbnb Inc. All rights reserved.

import UIKit
import SwiftUI

import StaticLib
import SwiftDylib
import ObjcDylib

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {
  var window: UIWindow?

  func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
    let window = UIWindow(frame: UIScreen.main.bounds)

    callMethods()

    if #available(iOS 13.0, *) {
      window.rootViewController = UIHostingController(rootView: SwiftUIView())
    } else {
      window.rootViewController = ViewController()
    }
    // Show the window
    window.makeKeyAndVisible()
    self.window = window
    return true
  }

  func callMethods() {
    // StaticLib
    let _ = BarClass()

    // SwiftDylib
    sayHelloFromSwiftDylib()

    // ObjcDylib
    let objcDylib = LLIOSObjcDylib()
    objcDylib.sayHello("LLIOS")
  }
}

