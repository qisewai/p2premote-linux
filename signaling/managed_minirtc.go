package main

import "encoding/json"

type miniRTCSignal struct {
	Type           string  `json:"type"`
	Status         string  `json:"status,omitempty"`
	UserID         string  `json:"user_id,omitempty"`
	RemoteUserID   string  `json:"remote_user_id,omitempty"`
	TransmissionID string  `json:"transmission_id"`
	SDP            string  `json:"sdp,omitempty"`
	Candidate      string  `json:"candidate,omitempty"`
	MID            *string `json:"mid,omitempty"`
	Ufrag          *string `json:"ufrag,omitempty"`
}

// The account socket has already authenticated both devices and authorized this
// room. MiniRTC's host/viewer IDs are local to the room, never account identities.
// Reconstruct each message so the peer cannot supply identity or ICE credentials.
func relayMiniRTC(r *room, from *client, m message) bool {
	if m.Type != "minirtc" || len(m.Payload) > 64*1024 ||
		(from != r.host && from != r.viewer) {
		return false
	}
	var incoming miniRTCSignal
	if json.Unmarshal([]byte(m.Payload), &incoming) != nil {
		return false
	}
	sender, target := "viewer", r.host
	if from == r.host {
		sender, target = "host", r.viewer
	}
	outgoing := miniRTCSignal{Type: incoming.Type, TransmissionID: "host", RemoteUserID: sender}
	switch incoming.Type {
	case "join_transmission":
		if from != r.viewer || r.stage != "accepted" || r.miniJoined {
			return false
		}
		r.miniJoined = true
		outgoing = miniRTCSignal{Type: "user_join_transmission", Status: "success",
			UserID: sender, TransmissionID: "host"}
	case "offer":
		if from != r.host || !r.miniJoined || r.stage != "accepted" || incoming.SDP == "" {
			return false
		}
		r.stage = "offered"
		outgoing.SDP = incoming.SDP
	case "answer":
		if from != r.viewer || r.stage != "offered" || incoming.SDP == "" {
			return false
		}
		r.stage = "answered"
		outgoing.SDP = incoming.SDP
	case "new_candidate", "new_candidate_mid":
		if !r.miniJoined || len(m.Payload) > 4096 {
			return false
		}
		if incoming.Type == "new_candidate" && incoming.SDP == "" ||
			incoming.Type == "new_candidate_mid" && (incoming.MID == nil || incoming.Candidate == "") {
			return false
		}
		outgoing.SDP, outgoing.Candidate, outgoing.MID, outgoing.Ufrag =
			incoming.SDP, incoming.Candidate, incoming.MID, incoming.Ufrag
	default:
		return false
	}
	payload, err := json.Marshal(outgoing)
	if err != nil {
		return false
	}
	target.send("minirtc", string(payload))
	return true
}
