import SwiftUI
import UIKit
import AVFoundation
import AVKit
import Combine

// =====================================================================
// 1) CONFIG AZURACAST
// - URLs base, API y stream de tu emisora
// =====================================================================
enum AZ {
    static let host = "https://radio.prorfws.com"
    static let baseURL = URL(string: host)!
    static let apiBase = URL(string: "\(host)/api/")!   // ✅ trailing slash

    // Stream real
    static let streamURL = URL(string: "\(host)/listen/loop-station/radio.mp3")!

    // Slug público
    static let stationPublicSlug = "loop-station"

    // API Key opcional (vacío si no se usa)
    static let apiKey: String = ""
}

// =====================================================================
// 2) HELPERS DE RED (JSON) + DETECCIÓN DE HTML
// - makeJSONRequest: arma URLRequest con headers y Bearer si hay apiKey
// - isHTML: evita que el parser intente leer HTML como JSON
// =====================================================================
private func makeJSONRequest(_ url: URL, method: String = "GET") -> URLRequest {
    var req = URLRequest(url: url)
    req.httpMethod = method
    req.timeoutInterval = 15
    req.cachePolicy = .reloadIgnoringLocalCacheData
    req.setValue("application/json", forHTTPHeaderField: "Accept")
    req.setValue("LoopStationApp/1.0 (iOS)", forHTTPHeaderField: "User-Agent")

    let key = AZ.apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
    if !key.isEmpty {
        req.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization")
    }
    return req
}

private func isHTML(_ data: Data) -> Bool {
    guard let s = String(data: data, encoding: .utf8)?.lowercased() else { return false }
    return s.contains("<html") || s.contains("<!doctype html")
}

// =====================================================================
// 3) HELPERS UI
// - hideKeyboard(): cierra teclado
// =====================================================================
private extension View {
    func hideKeyboard() {
        UIApplication.shared.sendAction(#selector(UIResponder.resignFirstResponder),
                                        to: nil, from: nil, for: nil)
    }
}

// =====================================================================
// 4) STATION RESOLVER
// - Resuelve stationID usando nowplaying/nowplaying_static; si falla intenta stations
// - Se usa para Requests (necesita stationID)
// =====================================================================
@MainActor
final class StationResolver: ObservableObject {
    static let shared = StationResolver()

    @Published private(set) var stationID: Int?
    @Published private(set) var lastError: String?

    private var task: URLSessionDataTask?

    func resolve(force: Bool = false) {
        if stationID != nil, !force { return }

        task?.cancel()
        lastError = nil

        let dash = AZ.stationPublicSlug
        let underscore = dash.replacingOccurrences(of: "-", with: "_")

        let urls: [URL] = [
            URL(string: "nowplaying/\(dash)", relativeTo: AZ.apiBase)!,
            URL(string: "nowplaying/\(underscore)", relativeTo: AZ.apiBase)!,
            URL(string: "nowplaying_static/\(dash).json", relativeTo: AZ.apiBase)!,
            URL(string: "nowplaying_static/\(underscore).json", relativeTo: AZ.apiBase)!
        ]

        resolveFromNowPlaying(urls: urls, index: 0)
    }

    private func resolveFromNowPlaying(urls: [URL], index: Int) {
        guard index < urls.count else {
            resolveFromStations()
            return
        }

        let url = urls[index]
        let req = makeJSONRequest(url)

        task = URLSession.shared.dataTask(with: req) { data, resp, err in
            Task { @MainActor in
                let code = (resp as? HTTPURLResponse)?.statusCode ?? -1

                if let err {
                    self.lastError = "nowplaying error: \(err.localizedDescription)"
                    self.resolveFromNowPlaying(urls: urls, index: index + 1)
                    return
                }

                guard let data, !data.isEmpty else {
                    self.lastError = "nowplaying vacío (HTTP \(code))"
                    self.resolveFromNowPlaying(urls: urls, index: index + 1)
                    return
                }

                if isHTML(data) {
                    self.lastError = "nowplaying devolvió HTML (HTTP \(code))"
                    self.resolveFromNowPlaying(urls: urls, index: index + 1)
                    return
                }

                guard let any = try? JSONSerialization.jsonObject(with: data) else {
                    self.lastError = "nowplaying no JSON (HTTP \(code))"
                    self.resolveFromNowPlaying(urls: urls, index: index + 1)
                    return
                }

                let json: [String: Any]?
                if let d = any as? [String: Any] { json = d }
                else if let a = any as? [[String: Any]] { json = a.first }
                else { json = nil }

                guard let json else {
                    self.lastError = "nowplaying formato inesperado"
                    self.resolveFromNowPlaying(urls: urls, index: index + 1)
                    return
                }

                if let station = json["station"] as? [String: Any] {
                    if let id = station["id"] as? Int { self.stationID = id; self.lastError = nil; return }
                    if let idStr = station["id"] as? String, let id = Int(idStr) { self.stationID = id; self.lastError = nil; return }
                }

                if let id = json["station_id"] as? Int {
                    self.stationID = id; self.lastError = nil; return
                }

                self.resolveFromNowPlaying(urls: urls, index: index + 1)
            }
        }
        task?.resume()
    }

    private func resolveFromStations() {
        task?.cancel()
        lastError = nil

        let url = URL(string: "stations", relativeTo: AZ.apiBase)!
        let req = makeJSONRequest(url)

        task = URLSession.shared.dataTask(with: req) { data, resp, err in
            Task { @MainActor in
                let code = (resp as? HTTPURLResponse)?.statusCode ?? -1

                if let err {
                    self.lastError = "stations error: \(err.localizedDescription)"
                    return
                }
                guard let data else {
                    self.lastError = "stations vacío (HTTP \(code))"
                    return
                }
                if isHTML(data) {
                    self.lastError = "stations devolvió HTML (HTTP \(code))"
                    return
                }
                guard (try? JSONSerialization.jsonObject(with: data)) is [[String: Any]] else {
                    self.lastError = "stations no JSON (HTTP \(code))"
                    return
                }

                self.lastError = "No pude resolver station_id."
            }
        }
        task?.resume()
    }
}

// =====================================================================
// 5) RADIO PLAYER VM
// - Maneja AVPlayer, reconexiones, nowplaying, historial, tiempos, notificaciones
// =====================================================================
final class RadioPlayerVM: ObservableObject {
    private var wasInterrupted: Bool = false

    let player = AVPlayer()
    @Published var isPlaying = false
    private var isStarting = false

    @Published var nowPlaying: String = "Cargando..."
    @Published var artist: String = ""
    @Published var coverUrl: String?
    @Published var playlistName: String = ""

    @Published var elapsed: Int = 0
    @Published var duration: Int = 0
    private var lastSongKey: String = ""

    struct SongEntry: Identifiable {
        let id = UUID()
        let title: String
        let artist: String
    }
    @Published var history: [SongEntry] = []

    private var stalledSeconds: Int = 0
    private var notAdvancingSeconds: Int = 0
    private var retryCount: Int = 0
    private var lastRestart: Date = .distantPast
    private var lastPlayerSeconds: Double = -1

    private var timeObserverToken: Any?
    private var ncTokens: [NSObjectProtocol] = []
    private var didInstallNotifs = false

    private var playbackTick: AnyCancellable?
    private var apiTick: AnyCancellable?

    private var nowPlayingTask: URLSessionDataTask?

    deinit {
        nowPlayingTask?.cancel()
        stopTimers()
        cleanupObservers()
    }

    func onAppear() {
        installNotificationsIfNeeded()
        installTimeObserverIfNeeded()
        startTimers()
        fetchNowPlaying()
    }

    func togglePlay() { isPlaying ? stopStream() : startStream() }

    func ensurePlayingForAirPlay() {
        if !isPlaying { startStream() }

        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.allowAirPlay, .allowBluetoothA2DP])
        try? session.setActive(true)

        player.allowsExternalPlayback = true
        player.usesExternalPlaybackWhileExternalScreenIsActive = true

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
            if self.isPlaying { self.player.play() }
        }
    }

    func startStream() {
        guard !isStarting else { return }
        isStarting = true

        player.pause()
        player.replaceCurrentItem(with: nil)

        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.allowAirPlay, .allowBluetoothA2DP])
        try? session.setActive(true)

        player.allowsExternalPlayback = true
        player.usesExternalPlaybackWhileExternalScreenIsActive = true

        let item = AVPlayerItem(url: AZ.streamURL)
        item.preferredForwardBufferDuration = 15
        item.canUseNetworkResourcesForLiveStreamingWhilePaused = true

        player.automaticallyWaitsToMinimizeStalling = true
        player.replaceCurrentItem(with: item)
        player.play()

        isPlaying = true
        stalledSeconds = 0
        notAdvancingSeconds = 0
        retryCount = 0
        lastPlayerSeconds = -1

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { self.isStarting = false }
    }

    func stopStream() {
        isPlaying = false
        player.pause()
        player.replaceCurrentItem(with: nil)

        elapsed = 0
        duration = 0
        stalledSeconds = 0
        notAdvancingSeconds = 0
        retryCount = 0
        lastPlayerSeconds = -1

        let session = AVAudioSession.sharedInstance()
        try? session.setActive(false, options: [.notifyOthersOnDeactivation])
    }

    private func monitorPlayer() {
        guard isPlaying else { return }
        guard let item = player.currentItem else { scheduleReconnect("no-item"); return }
        if item.status == .failed { scheduleReconnect("item.failed"); return }

        if player.timeControlStatus == .waitingToPlayAtSpecifiedRate || item.isPlaybackBufferEmpty {
            stalledSeconds += 1
            if stalledSeconds == 1 { player.play() }
            notAdvancingSeconds = 0
            if stalledSeconds >= 25 {
                stalledSeconds = 0
                scheduleReconnect("buffer-empty/waiting")
            }
            return
        }

        let current = player.currentTime().seconds
        if current.isFinite {
            if current == lastPlayerSeconds && player.timeControlStatus == .playing {
                notAdvancingSeconds += 1
            } else {
                notAdvancingSeconds = 0
            }
            lastPlayerSeconds = current
        }

        if notAdvancingSeconds >= 8 {
            if duration > 0 {
                notAdvancingSeconds = 0
                scheduleReconnect("no-advance")
            } else {
                notAdvancingSeconds = 0
                retryCount = 0
            }
        } else {
            retryCount = 0
        }
    }

    private func scheduleReconnect(_ reason: String = "") {
        guard isPlaying else { return }
        if Date().timeIntervalSince(lastRestart) < 5 { return }
        lastRestart = Date()

        retryCount += 1
        let delay = min(20, 2 * retryCount)

        DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(delay)) {
            guard self.isPlaying else { return }
            self.startStream()
        }
    }

    private func startTimers() {
        guard playbackTick == nil, apiTick == nil else { return }

        playbackTick = Timer.publish(every: 1, on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in
                guard let self else { return }
                self.updatePlaybackTime()
                self.monitorPlayer()
            }

        apiTick = Timer.publish(every: 10, on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in self?.fetchNowPlaying() }
    }

    private func stopTimers() {
        playbackTick?.cancel(); playbackTick = nil
        apiTick?.cancel(); apiTick = nil
    }

    private func installTimeObserverIfNeeded() {
        guard timeObserverToken == nil else { return }
        timeObserverToken = player.addPeriodicTimeObserver(
            forInterval: CMTime(seconds: 1, preferredTimescale: 2),
            queue: .main
        ) { _ in }
    }

    private func installNotificationsIfNeeded() {
        guard !didInstallNotifs else { return }
        didInstallNotifs = true

        ncTokens.append(
            NotificationCenter.default.addObserver(
                forName: .AVPlayerItemPlaybackStalled,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                guard let self, self.isPlaying else { return }
                self.player.play()
                DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                    guard self.isPlaying else { return }
                    if self.player.timeControlStatus != .playing {
                        self.scheduleReconnect("playback-stalled")
                    }
                }
            }
        )

        ncTokens.append(
            NotificationCenter.default.addObserver(
                forName: AVAudioSession.interruptionNotification,
                object: nil,
                queue: .main
            ) { [weak self] n in
                guard let self else { return }
                guard let info = n.userInfo,
                      let typeValue = info[AVAudioSessionInterruptionTypeKey] as? UInt,
                      let type = AVAudioSession.InterruptionType(rawValue: typeValue)
                else { return }

                switch type {
                case .began:
                    self.wasInterrupted = true

                case .ended:
                    guard self.isPlaying else { self.wasInterrupted = false; return }

                    let session = AVAudioSession.sharedInstance()
                    try? session.setActive(true)

                    if self.player.currentItem != nil {
                        self.player.play()
                    } else {
                        self.startStream()
                    }
                    self.wasInterrupted = false

                @unknown default:
                    break
                }
            }
        )

        ncTokens.append(
            NotificationCenter.default.addObserver(
                forName: AVAudioSession.routeChangeNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                guard let self, self.isPlaying else { return }

                let session = AVAudioSession.sharedInstance()
                try? session.setActive(true)

                DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                    if self.player.currentItem != nil {
                        self.player.play()
                    } else {
                        self.startStream()
                    }
                }
            }
        )
    }

    private func cleanupObservers() {
        if let tok = timeObserverToken {
            player.removeTimeObserver(tok)
            timeObserverToken = nil
        }
        for t in ncTokens { NotificationCenter.default.removeObserver(t) }
        ncTokens.removeAll()
        didInstallNotifs = false
    }

    private func candidateNowPlayingURLs() -> [URL] {
        let dash = AZ.stationPublicSlug
        let underscore = dash.replacingOccurrences(of: "-", with: "_")

        let paths = [
            "api/nowplaying/\(dash)",
            "api/nowplaying/\(underscore)",
            "api/nowplaying_static/\(dash).json",
            "api/nowplaying_static/\(underscore).json"
        ]
        return paths.compactMap { URL(string: $0, relativeTo: AZ.baseURL) }
    }

    func fetchNowPlaying() {
        nowPlayingTask?.cancel()
        fetchNowPlaying(from: candidateNowPlayingURLs(), index: 0)
    }

    private func fetchNowPlaying(from urls: [URL], index: Int) {
        guard index < urls.count else { return }
        let url = urls[index]
        let req = makeJSONRequest(url)

        nowPlayingTask = URLSession.shared.dataTask(with: req) { [weak self] data, _, _ in
            guard let self else { return }

            guard let data, !data.isEmpty, !isHTML(data) else {
                self.fetchNowPlaying(from: urls, index: index + 1)
                return
            }

            guard let jsonAny = try? JSONSerialization.jsonObject(with: data, options: []) else {
                self.fetchNowPlaying(from: urls, index: index + 1)
                return
            }

            let json: [String: Any]?
            if let dict = jsonAny as? [String: Any] { json = dict }
            else if let arr = jsonAny as? [[String: Any]] { json = arr.first }
            else { json = nil }

            guard let json, json["now_playing"] != nil else {
                self.fetchNowPlaying(from: urls, index: index + 1)
                return
            }

            DispatchQueue.main.async { self.applyNowPlayingJSON(json) }
        }
        nowPlayingTask?.resume()
    }

    private func applyNowPlayingJSON(_ json: [String: Any]) {
        guard let np = json["now_playing"] as? [String: Any],
              let song = np["song"] as? [String: Any]
        else { return }

        let title = (song["title"] as? String) ?? ""
        let art = (song["artist"] as? String) ?? ""
        let key = "\(title)|\(art)"

        nowPlaying = title.isEmpty ? "—" : title
        artist = art
        coverUrl = song["art"] as? String

        if let pl = np["playlist"] as? String { playlistName = pl } else { playlistName = "" }

        let d1 = np["duration"] as? Int ?? 0
        let d2 = song["duration"] as? Int ?? 0
        duration = (d1 > 0 ? d1 : d2)

        let playedAt = np["played_at"] as? Int ?? 0
        if duration > 0, playedAt > 0 {
            let now = Int(Date().timeIntervalSince1970)
            let calc = max(0, min(duration, now - playedAt))

            if key != lastSongKey {
                lastSongKey = key
                elapsed = calc
            } else {
                elapsed = max(elapsed, calc)
            }
        } else {
            let apiElapsed = np["elapsed"] as? Int ?? 0
            if key != lastSongKey {
                lastSongKey = key
                elapsed = apiElapsed
            } else if apiElapsed > 0 {
                elapsed = apiElapsed
            }
        }

        if let hist = json["song_history"] as? [[String: Any]] {
            history = hist.prefix(30).compactMap {
                guard let s = $0["song"] as? [String: Any] else { return nil }
                return SongEntry(title: s["title"] as? String ?? "", artist: s["artist"] as? String ?? "")
            }
        }
    }

    private func updatePlaybackTime() {
        guard isPlaying, duration > 0 else { return }
        elapsed = min(elapsed + 1, duration)
    }
}

// =====================================================================
// 6) REQUESTS MODELS
// - RequestSong: modelo que se muestra en lista
// - RequestsVM: API de AzuraCast requests + paginación + POST request
// =====================================================================
struct RequestSong: Identifiable {
    let id: String
    let title: String
    let artist: String
    let artURL: String?
}

@MainActor
final class RequestsVM: ObservableObject {
    @Published var query: String = ""
    @Published var isLoading = false
    @Published var errorText: String?
    @Published var items: [RequestSong] = []

    @Published var page: Int = 1
    @Published var totalPages: Int = 1
    private let perPage: Int = 10

    private var task: URLSessionDataTask?
    private var retriesLeft: Int = 10

    func onAppear() {
        retriesLeft = 10
        StationResolver.shared.resolve(force: false)
        fetch()
    }

    func refresh() { page = 1; fetch() }
    func nextPage() { guard page < totalPages else { return }; page += 1; fetch() }
    func prevPage() { guard page > 1 else { return }; page -= 1; fetch() }

    func fetch() {
        task?.cancel()

        guard let sid = StationResolver.shared.stationID else {
            isLoading = false
            if retriesLeft > 0 {
                retriesLeft -= 1
                errorText = "Buscando estación…"
                StationResolver.shared.resolve(force: true)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in self?.fetch() }
            } else {
                errorText = "No pude resolver la estación. \(StationResolver.shared.lastError ?? "")"
            }
            return
        }

        isLoading = true
        errorText = nil

        let q = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let base = URL(string: "station/\(sid)/requests", relativeTo: AZ.apiBase)!

        func urlWith(_ param: String) -> URL {
            var comps = URLComponents(url: base, resolvingAgainstBaseURL: true)!
            var items: [URLQueryItem] = [
                .init(name: "page", value: "\(page)"),
                .init(name: "per_page", value: "\(perPage)")
            ]
            if !q.isEmpty { items.append(.init(name: param, value: q)) }
            comps.queryItems = items
            return comps.url!
        }

        let urls: [URL] = q.isEmpty ? [urlWith("search")] : [urlWith("searchPhrase"), urlWith("search")]
        tryFetch(urls, idx: 0)
    }

    private func tryFetch(_ urls: [URL], idx: Int) {
        guard idx < urls.count else {
            isLoading = false
            if items.isEmpty { errorText = "No se encontraron canciones." }
            return
        }

        let req = makeJSONRequest(urls[idx])
        task = URLSession.shared.dataTask(with: req) { data, resp, err in
            Task { @MainActor in
                let code = (resp as? HTTPURLResponse)?.statusCode ?? -1

                if code == 404 {
                    self.tryFetch(urls, idx: idx + 1)
                    return
                }

                if let err {
                    self.isLoading = false
                    self.items = []
                    self.errorText = "Error: \(err.localizedDescription)"
                    return
                }

                guard let data, !data.isEmpty else {
                    self.isLoading = false
                    self.items = []
                    self.errorText = "Sin datos (HTTP \(code))"
                    return
                }

                if isHTML(data) {
                    self.isLoading = false
                    self.items = []
                    self.errorText = "El servidor devolvió HTML (no JSON)."
                    return
                }

                guard let any = try? JSONSerialization.jsonObject(with: data) else {
                    self.isLoading = false
                    self.items = []
                    self.errorText = "Respuesta inválida (HTTP \(code))"
                    return
                }

                let parsed = self.parseRequestsResponse(any)
                self.items = parsed.items
                self.totalPages = max(1, parsed.totalPages)
                self.isLoading = false

                if self.items.isEmpty {
                    if self.query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false {
                        self.tryFetch(urls, idx: idx + 1)
                    } else {
                        self.errorText = "No hay canciones disponibles para pedir."
                    }
                } else {
                    self.errorText = nil
                }
            }
        }
        task?.resume()
    }

    private func parseRequestsResponse(_ any: Any) -> (items: [RequestSong], totalPages: Int) {
        var rows: [[String: Any]] = []
        var pages = 1

        if let dict = any as? [String: Any] {
            if let r = dict["rows"] as? [[String: Any]] { rows = r }
            else if let data = dict["data"] as? [[String: Any]] { rows = data }
            else if let reqs = dict["requests"] as? [[String: Any]] { rows = reqs }

            pages = dict["total_pages"] as? Int
                ?? (dict["pagination"] as? [String: Any])?["total_pages"] as? Int
                ?? pages
        } else if let arr = any as? [[String: Any]] {
            rows = arr
        }

        let items: [RequestSong] = rows.compactMap { row in
            let song = (row["song"] as? [String: Any]) ?? row
            let requestID = (row["request_id"] as? String) ?? (song["request_id"] as? String) ?? ""
            guard !requestID.isEmpty else { return nil }

            let title = (song["title"] as? String) ?? ""
            let artist = (song["artist"] as? String) ?? ""
            let art = (song["art"] as? String)

            return RequestSong(id: requestID, title: title, artist: artist, artURL: art)
        }

        return (items, pages)
    }

    func request(_ song: RequestSong) {
        task?.cancel()
        isLoading = true
        errorText = nil

        guard let sid = StationResolver.shared.stationID else {
            isLoading = false
            errorText = "Estación no resuelta."
            return
        }

        let url = URL(string: "station/\(sid)/request/\(song.id)", relativeTo: AZ.apiBase)!
        let req = makeJSONRequest(url, method: "POST")

        task = URLSession.shared.dataTask(with: req) { data, resp, err in
            Task { @MainActor in
                let code = (resp as? HTTPURLResponse)?.statusCode ?? -1
                self.isLoading = false

                if let err {
                    self.errorText = "No se pudo solicitar: \(err.localizedDescription)"
                    return
                }

                if code >= 400 {
                    if let data,
                       let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                       let errMsg = obj["error"] as? String {
                        self.errorText = errMsg
                    } else {
                        self.errorText = "No se pudo solicitar (HTTP \(code))."
                    }
                    return
                }

                self.errorText = "✅ Solicitud enviada"
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { [weak self] in self?.errorText = nil }
            }
        }
        task?.resume()
    }
}

// =====================================================================
// 7) AIRPLAY BUTTON (UIViewRepresentable)
// - Botón nativo para AirPlay devices
// =====================================================================
struct AirPlayButtonView: UIViewRepresentable {
    let onWillOpen: () -> Void
    func makeCoordinator() -> Coordinator { Coordinator(onWillOpen: onWillOpen) }

    func makeUIView(context: Context) -> AVRoutePickerView {
        let picker = AVRoutePickerView(frame: .zero)
        picker.translatesAutoresizingMaskIntoConstraints = false
        picker.activeTintColor = .white
        picker.tintColor = .white
        picker.backgroundColor = .clear
        picker.prioritizesVideoDevices = false
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIView(_ picker: AVRoutePickerView, context: Context) {
        context.coordinator.onWillOpen = onWillOpen
        if picker.delegate == nil { picker.delegate = context.coordinator }
    }

    final class Coordinator: NSObject, AVRoutePickerViewDelegate {
        var onWillOpen: (() -> Void)?
        init(onWillOpen: @escaping () -> Void) { self.onWillOpen = onWillOpen }
        func routePickerViewWillBeginPresentingRoutes(_ routePickerView: AVRoutePickerView) { onWillOpen?() }
    }
}

// =====================================================================
// 8) STROBE (la animación roja del Home)
// =====================================================================
struct StrobeStrip: View {
    let isActive: Bool
    private let dotSizes: [CGFloat] = [3.5, 4.5, 5.5, 6.5]
    private let gaps:     [CGFloat] = [6.0, 7.0, 8.0, 9.0]
    private let pxPerSec: [CGFloat] = [220, 140, 90, 55]
    @State private var accumulated: TimeInterval = 0
    @State private var startedAt: Date = Date()

    var body: some View {
        TimelineView(.animation(minimumInterval: 1/60)) { timeline in
            let t = isActive ? (accumulated + timeline.date.timeIntervalSince(startedAt)) : accumulated
            Canvas { ctx, size in
                let rowSpacing: CGFloat = 6
                let maxDot = dotSizes.max() ?? 6
                for row in 0..<4 {
                    let dot = dotSizes[row]
                    let gap = gaps[row]
                    let pattern = dot + gap
                    let y = CGFloat(row) * (maxDot + rowSpacing) + dot / 2
                    let speed = pxPerSec[row]
                    let dx = -CGFloat((t * Double(speed)).truncatingRemainder(dividingBy: Double(pattern)))
                    var x = dx - pattern * 10
                    let end = size.width + pattern * 10
                    while x < end {
                        let rect = CGRect(x: x, y: y - dot/2, width: dot, height: dot)
                        ctx.fill(Path(ellipseIn: rect), with: .color(.red.opacity(0.92)))
                        x += pattern
                    }
                }
            }
        }
        .frame(height: 48)
        .mask(LinearGradient(gradient: Gradient(colors: [.clear,.black,.black,.clear]),
                             startPoint: .leading, endPoint: .trailing))
        .blendMode(.screen)
        .shadow(color: .red.opacity(0.8), radius: 6)
        .allowsHitTesting(false)
        .onAppear { startedAt = Date(); accumulated = 0 }
        .onChange(of: isActive) { _, playing in
            if playing { startedAt = Date() } else { accumulated += Date().timeIntervalSince(startedAt) }
        }
    }
}

// =====================================================================
// 9) NAVEGACIÓN PRINCIPAL (Tabs + BottomBar)
// =====================================================================
enum AppTab { case home, history, requests }

struct BottomBar: View {
    @Binding var tab: AppTab
    let onAirPlay: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            Rectangle().fill(Color.white.opacity(0.15)).frame(height: 1)

            HStack {
                barButton(icon: "house", selected: tab == .home) { tab = .home }
                Spacer()
                barButton(icon: "line.3.horizontal", selected: tab == .history) { tab = .history }
                Spacer()
                barButton(icon: "music.note.list", selected: tab == .requests) { tab = .requests }
                Spacer()

                AirPlayButtonView { onAirPlay() }
                    .frame(width: 36, height: 36)
                    .opacity(0.95)
            }
            .padding(.horizontal, 34)
            .padding(.top, 12)
            .padding(.bottom, 10)
        }
    }

    private func barButton(icon: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 22, weight: .regular))
                .foregroundColor(Color.white.opacity(selected ? 1.0 : 0.55))
                .frame(width: 44, height: 36)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

struct RootView: View {
    @StateObject private var playerVM = RadioPlayerVM()
    @State private var tab: AppTab = .home

    var body: some View {
        ZStack {
            switch tab {
            case .home: HomeView(vm: playerVM)
            case .history: HistoryView(vm: playerVM)
            case .requests: RequestsView()
            }
        }
        .safeAreaInset(edge: .bottom) {
            BottomBar(tab: $tab) { playerVM.ensurePlayingForAirPlay() }
                .background(Color.clear)
        }
        .onAppear { StationResolver.shared.resolve(force: false) }
    }
}

// =====================================================================
// 10) HOME SCREEN
// =====================================================================
struct HomeView: View {
    @ObservedObject var vm: RadioPlayerVM
    @Environment(\.horizontalSizeClass) private var hSize

    var body: some View {
        ZStack {
            Image("BackgroundTexture").resizable().scaledToFill().ignoresSafeArea()

            VStack(spacing: 0) {
                Spacer()

                Text("Loop Station")
                    .font(.system(size: 32, weight: .semibold))
                    .foregroundColor(Color.white.opacity(0.88))
                    .multilineTextAlignment(.center)
                    .lineLimit(1)
                    .minimumScaleFactor(0.75)
                    .allowsTightening(true)
                    .frame(maxWidth: .infinity)
                    .padding(.horizontal, 20)
                    .padding(.bottom, 10)

                Group { if hSize == .compact { portraitLayout } else { landscapeLayout } }

                Spacer()
            }
            .padding(.horizontal, 16)
            .safeAreaPadding(.top, 10)
        }
        .onAppear { vm.onAppear() }
    }

    private var portraitLayout: some View {
        VStack(spacing: 22) {
            coverView(size: 220)
            nowPlayingView
            progressView
            playButton
        }
    }

    private var landscapeLayout: some View {
        HStack(spacing: 28) {
            coverView(size: 280)
            VStack(spacing: 18) {
                nowPlayingView
                progressView
                playButton
            }
            .frame(maxWidth: 380)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .center)
        .padding(.top, 4)
    }

    private func coverView(size: CGFloat) -> some View {
        Group {
            if let coverUrl = vm.coverUrl, let url = URL(string: coverUrl) {
                AsyncImage(url: url) { img in img.resizable().scaledToFill() }
                placeholder: { ProgressView() }
            } else {
                Image("LoopLogo").resizable().scaledToFill()
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: 30))
        .shadow(color: Color.black.opacity(0.35), radius: 18, y: 10)
    }

    private var nowPlayingView: some View {
        VStack(spacing: 6) {
            Text(vm.nowPlaying)
                .font(.title3.weight(.semibold))
                .foregroundColor(.white)
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .truncationMode(.tail)
                .minimumScaleFactor(0.70)
                .allowsTightening(true)
                .frame(maxWidth: .infinity)
                .padding(.horizontal, 20)

            Text(vm.artist)
                .font(.caption)
                .foregroundColor(.gray)
                .multilineTextAlignment(.center)
                .lineLimit(1)
                .truncationMode(.tail)
                .minimumScaleFactor(0.75)
                .allowsTightening(true)
                .frame(maxWidth: .infinity)
                .padding(.horizontal, 20)
        }
    }

    private var progressView: some View {
        VStack(spacing: 6) {
            StrobeStrip(isActive: vm.isPlaying)
                .frame(maxWidth: 320)
                .opacity(0.95)

            ProgressView(value: progressValue(), total: 1)
                .progressViewStyle(.linear)
                .tint(.white.opacity(0.75))
                .frame(maxWidth: 280, minHeight: 2, maxHeight: 2)
                .scaleEffect(y: 0.25)

            HStack {
                Text(formatTime(vm.elapsed)).lineLimit(1).minimumScaleFactor(0.8)
                Spacer()
                Text(vm.duration > 0 ? formatTime(vm.duration) : "--:--")
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .font(.caption2)
            .foregroundColor(.gray)
            .frame(maxWidth: 280)
        }
    }

    private var playButton: some View {
        Button { vm.togglePlay() } label: {
            Circle()
                .fill(Color.black.opacity(0.35))
                .frame(width: 64, height: 64)
                .overlay(
                    Image(systemName: vm.isPlaying ? "pause.fill" : "play.fill")
                        .font(.system(size: 22, weight: .semibold))
                        .foregroundColor(.white.opacity(0.9))
                )
                .overlay(Circle().stroke(Color.white.opacity(0.18), lineWidth: 1))
        }
        .buttonStyle(.plain)
    }

    private func formatTime(_ s: Int) -> String { String(format: "%02d:%02d", s / 60, s % 60) }
    private func progressValue() -> Double { vm.duration > 0 ? Double(vm.elapsed) / Double(vm.duration) : 0 }
}

// =====================================================================
// 11) HISTORY SCREEN  ✅ (SOLO UNA DEFINICIÓN)  <-- aquí estaba el error
// - Lista de historial
// - Padding bottom extra para no taparse con el BottomBar
// =====================================================================
struct HistoryView: View {
    @ObservedObject var vm: RadioPlayerVM

    var body: some View {
        GeometryReader { geo in
            ZStack {
                Image("BackgroundTexture").resizable().scaledToFill().ignoresSafeArea()

                VStack(alignment: .leading, spacing: 10) {
                    Text("Historial")
                        .font(.system(size: 22, weight: .semibold))
                        .foregroundColor(.white.opacity(0.92))
                        .lineLimit(1)
                        .minimumScaleFactor(0.8)

                    Text("Últimas canciones")
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.60))
                        .lineLimit(1)
                        .minimumScaleFactor(0.8)

                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 12) {
                            if vm.history.isEmpty {
                                Text("Cargando historial…")
                                    .foregroundColor(.gray)
                                    .padding(.top, 10)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                            } else {
                                ForEach(vm.history) { entry in
                                    VStack(alignment: .leading, spacing: 4) {
                                        Text(entry.title.isEmpty ? "—" : entry.title)
                                            .foregroundColor(.white)
                                            .font(.callout.weight(.semibold))
                                            .lineLimit(1)
                                            .truncationMode(.tail)
                                            .minimumScaleFactor(0.75)
                                            .allowsTightening(true)
                                            .frame(maxWidth: .infinity, alignment: .leading)
                                            .layoutPriority(1)

                                        Text(entry.artist.isEmpty ? " " : entry.artist)
                                            .foregroundColor(.gray)
                                            .font(.caption)
                                            .lineLimit(1)
                                            .truncationMode(.tail)
                                            .minimumScaleFactor(0.8)
                                            .allowsTightening(true)
                                            .frame(maxWidth: .infinity, alignment: .leading)
                                    }
                                    .padding(.vertical, 10)
                                    .padding(.horizontal, 12)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .background(
                                        RoundedRectangle(cornerRadius: 14)
                                            .fill(Color.black.opacity(0.35))
                                            .overlay(
                                                RoundedRectangle(cornerRadius: 14)
                                                    .stroke(Color.white.opacity(0.06), lineWidth: 1)
                                            )
                                    )
                                    .clipped()
                                }
                            }
                        }
                        .padding(.top, 10)
                        .padding(.bottom, 140)   // ✅ evita quedar debajo del BottomBar
                    }

                    Spacer(minLength: 0)
                }
                .frame(width: geo.size.width, alignment: .leading)
                .safeAreaPadding(.top, 10)
                .padding(.horizontal, 20)
            }
        }
        .onAppear { vm.fetchNowPlaying() }
    }
}

// =====================================================================
// 12) REQUESTS SCREEN ✅ (Dynamic Island + teclado NO empuja)
// - Sin GeometryReader (evita re-layout por teclado)
// - padding(.top, fixedTopInset + 8) fija el contenido debajo de la isla
// - ignoresSafeArea(.keyboard) evita que el teclado empuje el layout
// =====================================================================
struct RequestsView: View {
    @StateObject private var vm = RequestsVM()
    @FocusState private var isSearchFieldFocused: Bool
    @State private var fixedTopInset: CGFloat = 0

    var body: some View {
        ZStack {
            Image("BackgroundTexture")
                .resizable()
                .scaledToFill()
                .ignoresSafeArea()

            VStack(alignment: .leading, spacing: 12) {

                // --- Header
                HStack {
                    Text("    Choose your Track")
                        .font(.system(size: 22, weight: .semibold))
                        .foregroundColor(.white.opacity(0.92))
                        .lineLimit(1)
                        .minimumScaleFactor(0.8)

                    Spacer()

                    if vm.isLoading {
                        ProgressView().scaleEffect(0.9)
                    }
                }

                // --- Search
                ZStack(alignment: .leading) {
                    if vm.query.isEmpty {
                        Text("Search")
                            .foregroundColor(.white.opacity(0.55))
                            .padding(.horizontal, 20)
                            .padding(.vertical, 7)
                    }

                    TextField("", text: $vm.query)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 7)
                        .background(
                            RoundedRectangle(cornerRadius: 0)
                                .fill(Color.black.opacity(0.15))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 4)
                                        .stroke(Color.white.opacity(0.5), lineWidth: 0.3)
                                        .allowsHitTesting(false)
                                )
                        )
                        .foregroundColor(.white.opacity(0.85))
                        .submitLabel(.search)
                        .focused($isSearchFieldFocused)
                        .onSubmit {
                            isSearchFieldFocused = false
                            vm.refresh()
                        }
                }

                // --- Status / Error
                if let err = vm.errorText {
                    Text(err)
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.70))
                        .lineLimit(2)
                        .minimumScaleFactor(0.75)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }

                // --- List
                ScrollView(.vertical) {
                    LazyVStack(alignment: .leading, spacing: 10) {
                        ForEach(vm.items) { song in
                            requestRow(song)
                        }
                    }
                    .padding(.top, 6)
                    .padding(.bottom, 110)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .scrollDismissesKeyboard(.interactively)
                .clipped()
                .onTapGesture { isSearchFieldFocused = false }

                // --- Pagination
                HStack {
                    Button { vm.prevPage() } label: {
                        Image(systemName: "chevron.left")
                            .foregroundColor(.white.opacity(vm.page > 1 ? 0.9 : 0.25))
                    }
                    .buttonStyle(.plain)
                    .disabled(vm.page <= 1)

                    Text("\(vm.page) / \(vm.totalPages)")
                        .font(.caption.weight(.semibold))
                        .foregroundColor(.white.opacity(0.65))
                        .frame(minWidth: 70)

                    Button { vm.nextPage() } label: {
                        Image(systemName: "chevron.right")
                            .foregroundColor(.white.opacity(vm.page < vm.totalPages ? 0.9 : 0.25))
                    }
                    .buttonStyle(.plain)
                    .disabled(vm.page >= vm.totalPages)

                    Spacer()

                    Button {
                        isSearchFieldFocused = false
                        vm.fetch()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                            .foregroundColor(.white.opacity(0.85))
                    }
                    .buttonStyle(.plain)
                }
                .padding(.top, 2)
            }
            .padding(.horizontal, 20)
            .padding(.top, fixedTopInset + 8)                 // ✅ bajo Dynamic Island
            .frame(maxWidth: 560, alignment: .leading)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
        .ignoresSafeArea(.keyboard, edges: .all)             // ✅ teclado NO empuja
        .contentShape(Rectangle())
        .onTapGesture { isSearchFieldFocused = false }
        .toolbar {
            ToolbarItemGroup(placement: .keyboard) {
                Spacer()
                Button("Listo") { isSearchFieldFocused = false }
            }
        }
        .onAppear {
            vm.onAppear()
            DispatchQueue.main.async {
                fixedTopInset = Self.readTopSafeAreaInset()
            }
        }
    }

    private static func readTopSafeAreaInset() -> CGFloat {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let windows = scenes.flatMap { $0.windows }
        let key = windows.first(where: { $0.isKeyWindow }) ?? windows.first
        return key?.safeAreaInsets.top ?? 0
    }

    private func requestRow(_ song: RequestSong) -> some View {
        HStack(alignment: .center, spacing: 12) {
            Group {
                if let art = song.artURL, let url = URL(string: art) {
                    AsyncImage(url: url) { img in
                        img.resizable().scaledToFill()
                    } placeholder: {
                        Color.black.opacity(0.25)
                    }
                } else {
                    Color.black.opacity(0.25)
                        .overlay(
                            Image(systemName: "music.note")
                                .foregroundColor(.white.opacity(0.45))
                        )
                }
            }
            .frame(width: 48, height: 48)
            .clipShape(RoundedRectangle(cornerRadius: 10))

            VStack(alignment: .leading, spacing: 2) {
                Text(song.title.isEmpty ? "—" : song.title)
                    .font(.subheadline.weight(.semibold))
                    .foregroundColor(.white)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .minimumScaleFactor(0.70)
                    .allowsTightening(true)

                if !song.artist.isEmpty {
                    Text(song.artist)
                        .font(.caption)
                        .foregroundColor(.gray)
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .minimumScaleFactor(0.75)
                        .allowsTightening(true)
                }
            }
            .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
            .layoutPriority(1)

            Button {
                isSearchFieldFocused = false
                vm.request(song)
            } label: {
                Text("Send")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(.white)
                    .frame(width: 96, height: 36)
                    .background(
                        RoundedRectangle(cornerRadius: 0)
                            .fill(Color.black.opacity(0.45))
                            .overlay(
                                RoundedRectangle(cornerRadius: 4)
                                    .stroke(Color.white.opacity(0.30), lineWidth: 0.3)
                            )
                    )
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(
            RoundedRectangle(cornerRadius: 1)
                .fill(Color.black.opacity(0.15))
                .overlay(
                    RoundedRectangle(cornerRadius: 1)
                        .stroke(Color.white.opacity(0.06), lineWidth: 1)
                )
        )
        .clipped()
    }
}

// =====================================================================
// 13) APP ENTRY (ContentView)
// =====================================================================
struct ContentView: View {
    var body: some View {
        RootView()
    }
}

