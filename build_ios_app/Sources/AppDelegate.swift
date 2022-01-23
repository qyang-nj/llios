// Created by Qing Yang on 3/11/20.
// Copyright © 2020 Airbnb Inc. All rights reserved.

import UIKit
import SwiftUI
import os

import StaticLib
import SwiftDylib
import ObjcDylib
import MixedModule

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {
  var window: UIWindow?

  func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
    let window = UIWindow(frame: UIScreen.main.bounds)

    if #available(iOS 13.0, *) {
      window.rootViewController = UIHostingController(rootView: SwiftUIView())
    } else {
      window.rootViewController = ViewController()
    }
    // Show the window
    window.makeKeyAndVisible()
    self.window = window

    callMethods()

    return true
  }

  func callMethods() {
    let logger = Logger()

    // StaticLib
    let _ = BarClass()

    // SwiftDylib
    sayHelloFromSwiftDylib()

    // ObjcDylib
    let objcDylib = LLIOSObjcDylib()
    logger.log("### [ObjcDylib] \(objcDylib.message("Objc Dylib"))")

    // MixedModule
    let producer = MySwiftProducer()
    logger.log("### [MixedModule.MySwiftProducer] \(producer.product!.name)");

    let product = MyObjcProduct(name: "")
    logger.log("### [MixedModule.MyObjcProduct] \(product!.name)")
    logger.log("### [MixedModule.MyObjcProduct] \(product!.materialType())")
  }
}

