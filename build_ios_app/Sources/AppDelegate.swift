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
    callMethods()
    return true
  }

  func application(_ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let config = UISceneConfiguration(name: nil, sessionRole: connectingSceneSession.role)
    config.delegateClass = SceneDelegate.self
    return config
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
    logger.log("### [MixedModule.MyObjcProduct] \(product!.material)")
    logger.log("### [MixedModule.MyObjcProduct] \(product!.materialType())")
  }
}

