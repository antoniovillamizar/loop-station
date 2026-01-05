import SwiftUI

struct AppEntryView: View {
    @StateObject private var vm = RadioPlayerVM()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            VStack(spacing: 18) {
                Text("Loop Station")
                    .font(.system(size: 28, weight: .semibold))
                    .foregroundColor(.white)

                Text(vm.nowPlaying)
                    .font(.headline)
                    .foregroundColor(.white)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 16)

                Text(vm.artist)
                    .font(.caption)
                    .foregroundColor(.gray)

                Button {
                    vm.togglePlay()
                } label: {
                    Text(vm.isPlaying ? "PAUSE" : "PLAY")
                        .font(.system(size: 16, weight: .bold))
                        .foregroundColor(.white)
                        .frame(width: 160, height: 48)
                        .background(Color.blue.opacity(0.8))
                        .cornerRadius(14)
                }
            }
            .padding()
        }
        .onAppear { vm.onAppear() }
    }
}

