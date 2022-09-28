// Created by Qing Yang on 3/11/20.
// Copyright © 2020 Airbnb Inc. All rights reserved.

import UIKit

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {
  var window: UIWindow?

  func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
    let window = UIWindow(frame: UIScreen.main.bounds)
    window.rootViewController = ViewController()

    // Show the window
    window.makeKeyAndVisible()
    self.window = window

    return true
  }
}

