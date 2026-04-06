class Mactic < Formula
  desc "Send haptic waveforms and visualize multitouch input on MacBook trackpads"
  homepage "https://github.com/MatMercer/mactic"
  url "https://github.com/MatMercer/mactic/releases/download/v2.0/mactic-macos.tar.gz"
  version "2.0"
  sha256 "e30c7522c9e526fb22bbe693aecf1a51e7ee651f5f7d87f03058627fd312f85f"
  license "Unlicense"

  depends_on :macos

  def install
    bin.install "mactic"
  end

  test do
    assert_match "Usage:", shell_output("#{bin}/mactic -h 2>&1", 0)
  end
end
