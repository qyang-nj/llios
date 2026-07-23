//
//  AppShortcuts.swift
//  AppDemo
//

import AppIntents

struct SayHelloIntent: AppIntent {
    static let title: LocalizedStringResource = "Say Hello"
    static let description = IntentDescription("Get a friendly greeting from AppDemo.")

    @Parameter(title: "Name", description: "The name to include in the greeting.")
    var name: String?

    func perform() async throws -> some IntentResult & ProvidesDialog {
        let trimmedName = name?.trimmingCharacters(in: .whitespacesAndNewlines)
        let greeting = if let trimmedName, !trimmedName.isEmpty {
            "Hello, \(trimmedName)!"
        } else {
            "Hello from AppDemo!"
        }

        return .result(dialog: IntentDialog(stringLiteral: greeting))
    }
}

struct FlipCoinIntent: AppIntent {
    static let title: LocalizedStringResource = "Flip a Coin"
    static let description = IntentDescription("Flip a virtual coin in AppDemo.")

    func perform() async throws -> some IntentResult & ProvidesDialog {
        let result = Bool.random() ? "Heads" : "Tails"
        return .result(dialog: IntentDialog(stringLiteral: result))
    }
}

struct AppDemoShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: SayHelloIntent(),
            phrases: [
                "Say hello with \(.applicationName)",
                "Get a greeting from \(.applicationName)"
            ],
            shortTitle: "Say Hello",
            systemImageName: "hand.wave"
        )

        AppShortcut(
            intent: FlipCoinIntent(),
            phrases: [
                "Flip a coin with \(.applicationName)",
                "Ask \(.applicationName) to flip a coin"
            ],
            shortTitle: "Flip a Coin",
            systemImageName: "circle.lefthalf.filled"
        )
    }
}
