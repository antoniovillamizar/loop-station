import SwiftUI

struct HistoryView: View {
    @ObservedObject var vm: RadioPlayerVM
    private let sidePad: CGFloat = 20

    var body: some View {
        GeometryReader { geo in
            let contentW = max(0, geo.size.width - (sidePad * 2))

            ZStack {
                Image("BackgroundTexture")
                    .resizable()
                    .scaledToFill()
                    .ignoresSafeArea()

                VStack(alignment: .leading, spacing: 10) {

                    Text("Historial")
                        .font(.system(size: 22, weight: .semibold))
                        .foregroundColor(.white.opacity(0.92))

                    Text("Últimas canciones")
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.6))

                    ScrollView {
                        LazyVStack(spacing: 12) {
                            if vm.history.isEmpty {
                                Text("Cargando historial…")
                                    .foregroundColor(.gray)
                            } else {
                                ForEach(vm.history) { entry in
                                    VStack(alignment: .leading, spacing: 4) {
                                        Text(entry.title.isEmpty ? "—" : entry.title)
                                            .foregroundColor(.white)
                                            .font(.callout.weight(.semibold))
                                            .lineLimit(1)

                                        Text(entry.artist)
                                            .foregroundColor(.gray)
                                            .font(.caption)
                                            .lineLimit(1)
                                    }
                                    .padding()
                                    .frame(width: contentW, alignment: .leading)
                                    .background(Color.black.opacity(0.35))
                                    .cornerRadius(14)
                                }
                            }
                        }
                        .padding(.bottom, 90)
                    }
                }
                .padding(.horizontal, sidePad)
                .padding(.top, 10)
            }
        }
        .onAppear { vm.fetchNowPlaying() }
    }
}
