#pragma once
namespace CemuPad::Control::Detail {
inline constexpr const char* SchemaText=R"CEMUPAD_SCHEMA({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "Cemu Pad control protocol v1",
  "oneOf": [
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "HELLO"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "role": {
              "enum": [
                "host",
                "client"
              ]
            },
            "protocol_major": {
              "const": 1
            },
            "nonce": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            },
            "host_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            }
          },
          "required": [
            "role",
            "protocol_major",
            "nonce",
            "host_id"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "CAPABILITIES"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "input_hz": {
              "type": "integer",
              "minimum": 1,
              "maximum": 200
            },
            "gyro": {
              "type": "boolean"
            },
            "accel": {
              "type": "boolean"
            },
            "gyro_abs_max": {
              "type": "number",
              "minimum": 0.001,
              "maximum": 1000
            },
            "accel_abs_max": {
              "type": "number",
              "minimum": 0.001,
              "maximum": 10000
            },
            "haptics": {
              "type": "boolean"
            },
            "video_codecs": {
              "type": "array",
              "items": {
                "const": "H264"
              },
              "maxItems": 1,
              "uniqueItems": true
            },
            "audio_codecs": {
              "type": "array",
              "items": {
                "const": "Opus"
              },
              "maxItems": 1,
              "uniqueItems": true
            }
          },
          "required": [
            "input_hz",
            "gyro",
            "accel",
            "gyro_abs_max",
            "accel_abs_max",
            "haptics",
            "video_codecs",
            "audio_codecs"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "PAIR"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "token": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            },
            "client_certificate_der": {
              "type": "string",
              "pattern": "^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$",
              "minLength": 4,
              "maxLength": 16384,
              "not": {
                "pattern": "\\s"
              }
            },
            "client_nonce": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            },
            "proof_der": {
              "type": "string",
              "pattern": "^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$",
              "minLength": 4,
              "maxLength": 16384,
              "not": {
                "pattern": "\\s"
              }
            }
          },
          "required": [
            "token",
            "client_certificate_der",
            "client_nonce",
            "proof_der"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "PAIR_RESULT"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "status": {
              "const": "paired"
            },
            "client_spki_sha256": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            },
            "host_spki_sha256": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            }
          },
          "required": [
            "status",
            "client_spki_sha256",
            "host_spki_sha256"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "AUTH"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "peer_spki_sha256": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            },
            "control_binding": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            }
          },
          "required": [
            "peer_spki_sha256",
            "control_binding"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "AUTH_RESULT"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "status": {
              "const": "authenticated"
            },
            "client_spki_sha256": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            },
            "host_spki_sha256": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            }
          },
          "required": [
            "status",
            "client_spki_sha256",
            "host_spki_sha256"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "SESSION_START"
        },
        "request_id": {
          "const": "0"
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "game_epoch": {
              "type": "string",
              "pattern": "^(0|[1-9][0-9]{0,19})$",
              "minLength": 1,
              "maxLength": 20,
              "not": {
                "pattern": "\\s"
              }
            },
            "channels": {
              "type": "array",
              "items": {
                "type": "object",
                "properties": {
                  "channel": {
                    "enum": [
                      "input_haptics",
                      "video",
                      "audio"
                    ]
                  },
                  "address": {
                    "type": "string",
                    "minLength": 1,
                    "maxLength": 255
                  },
                  "port": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 65535
                  },
                  "dtls_role": {
                    "const": "server"
                  },
                  "host_spki_sha256": {
                    "type": "string",
                    "pattern": "^[0-9a-f]{64}$",
                    "minLength": 64,
                    "maxLength": 64
                  }
                },
                "required": [
                  "channel",
                  "address",
                  "port",
                  "dtls_role",
                  "host_spki_sha256"
                ],
                "additionalProperties": false
              },
              "minItems": 1,
              "maxItems": 3,
              "allOf": [
                {
                  "contains": {
                    "properties": {
                      "channel": {
                        "const": "input_haptics"
                      }
                    },
                    "required": [
                      "channel"
                    ]
                  },
                  "minContains": 1,
                  "maxContains": 1
                },
                {
                  "contains": {
                    "properties": {
                      "channel": {
                        "const": "video"
                      }
                    },
                    "required": [
                      "channel"
                    ]
                  },
                  "minContains": 0,
                  "maxContains": 1
                },
                {
                  "contains": {
                    "properties": {
                      "channel": {
                        "const": "audio"
                      }
                    },
                    "required": [
                      "channel"
                    ]
                  },
                  "minContains": 0,
                  "maxContains": 1
                }
              ]
            }
          },
          "required": [
            "session_id",
            "game_epoch",
            "channels"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "CHANNEL_READY"
        },
        "request_id": {
          "const": "0"
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "channel": {
              "enum": [
                "input_haptics",
                "video",
                "audio"
              ]
            },
            "dtls_exporter_sha256": {
              "type": "string",
              "pattern": "^[0-9a-f]{64}$",
              "minLength": 64,
              "maxLength": 64
            }
          },
          "required": [
            "session_id",
            "channel",
            "dtls_exporter_sha256"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "STREAM_CONFIG"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "oneOf": [
            {
              "type": "object",
              "properties": {
                "session_id": {
                  "type": "string",
                  "pattern": "^[0-9a-f]{32}$",
                  "minLength": 32,
                  "maxLength": 32,
                  "not": {
                    "const": "00000000000000000000000000000000"
                  }
                },
                "stream": {
                  "const": "video"
                },
                "generation": {
                  "type": "integer",
                  "minimum": 0,
                  "maximum": 4294967295
                },
                "ssrc": {
                  "type": "integer",
                  "minimum": 1,
                  "maximum": 4294967295
                },
                "codec": {
                  "const": "H264"
                },
                "rtp_payload_type": {
                  "const": 96
                },
                "packetization_mode": {
                  "const": 1
                },
                "profile": {
                  "const": "baseline"
                },
                "level": {
                  "type": "integer",
                  "minimum": 10,
                  "maximum": 52
                },
                "sps": {
                  "type": "string",
                  "pattern": "^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$",
                  "minLength": 4,
                  "maxLength": 16384,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "pps": {
                  "type": "string",
                  "pattern": "^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$",
                  "minLength": 4,
                  "maxLength": 16384,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "coded_width": {
                  "type": "integer",
                  "minimum": 2,
                  "maximum": 4096
                },
                "coded_height": {
                  "type": "integer",
                  "minimum": 2,
                  "maximum": 2160
                },
                "crop": {
                  "type": "object",
                  "properties": {
                    "left": {
                      "type": "integer",
                      "minimum": 0,
                      "maximum": 4095
                    },
                    "top": {
                      "type": "integer",
                      "minimum": 0,
                      "maximum": 2159
                    },
                    "width": {
                      "type": "integer",
                      "minimum": 1,
                      "maximum": 4096
                    },
                    "height": {
                      "type": "integer",
                      "minimum": 1,
                      "maximum": 2160
                    }
                  },
                  "required": [
                    "left",
                    "top",
                    "width",
                    "height"
                  ],
                  "additionalProperties": false
                },
                "color_matrix": {
                  "const": "BT709"
                },
                "color_range": {
                  "enum": [
                    "limited",
                    "full"
                  ]
                },
                "rtp_rate": {
                  "const": 90000
                },
                "rtp_base": {
                  "type": "integer",
                  "minimum": 0,
                  "maximum": 4294967295
                },
                "host_time_us": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                }
              },
              "required": [
                "session_id",
                "stream",
                "generation",
                "ssrc",
                "codec",
                "rtp_payload_type",
                "packetization_mode",
                "profile",
                "level",
                "sps",
                "pps",
                "coded_width",
                "coded_height",
                "crop",
                "color_matrix",
                "color_range",
                "rtp_rate",
                "rtp_base",
                "host_time_us"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "properties": {
                "session_id": {
                  "type": "string",
                  "pattern": "^[0-9a-f]{32}$",
                  "minLength": 32,
                  "maxLength": 32,
                  "not": {
                    "const": "00000000000000000000000000000000"
                  }
                },
                "stream": {
                  "const": "audio"
                },
                "generation": {
                  "type": "integer",
                  "minimum": 0,
                  "maximum": 4294967295
                },
                "ssrc": {
                  "type": "integer",
                  "minimum": 1,
                  "maximum": 4294967295
                },
                "codec": {
                  "const": "Opus"
                },
                "rtp_payload_type": {
                  "const": 111
                },
                "channels": {
                  "const": 2
                },
                "sample_rate": {
                  "const": 48000
                },
                "rtp_rate": {
                  "const": 48000
                },
                "packet_ms": {
                  "const": 10
                },
                "sample_base": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "rtp_base": {
                  "type": "integer",
                  "minimum": 0,
                  "maximum": 4294967295
                },
                "host_time_us": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                }
              },
              "required": [
                "session_id",
                "stream",
                "generation",
                "ssrc",
                "codec",
                "rtp_payload_type",
                "channels",
                "sample_rate",
                "rtp_rate",
                "packet_ms",
                "sample_base",
                "rtp_base",
                "host_time_us"
              ],
              "additionalProperties": false
            }
          ]
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "STREAM_READY"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "stream": {
              "enum": [
                "video",
                "audio"
              ]
            },
            "generation": {
              "type": "integer",
              "minimum": 0,
              "maximum": 4294967295
            },
            "ssrc": {
              "type": "integer",
              "minimum": 1,
              "maximum": 4294967295
            }
          },
          "required": [
            "session_id",
            "stream",
            "generation",
            "ssrc"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "HAPTIC_CONFIG"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "output_generation": {
              "type": "integer",
              "minimum": 0,
              "maximum": 4294967295
            },
            "route": {
              "enum": [
                "controller",
                "phone",
                "none"
              ]
            },
            "available": {
              "type": "boolean"
            },
            "lease_max_ms": {
              "const": 100
            }
          },
          "required": [
            "session_id",
            "output_generation",
            "route",
            "available",
            "lease_max_ms"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "HAPTIC_READY"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "output_generation": {
              "type": "integer",
              "minimum": 0,
              "maximum": 4294967295
            }
          },
          "required": [
            "session_id",
            "output_generation"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "HEARTBEAT"
        },
        "request_id": {
          "const": "0"
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "monotonic_us": {
              "type": "string",
              "pattern": "^(0|[1-9][0-9]{0,19})$",
              "minLength": 1,
              "maxLength": 20,
              "not": {
                "pattern": "\\s"
              }
            }
          },
          "required": [
            "session_id",
            "monotonic_us"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "TIME_SYNC"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "anyOf": [
              {
                "const": "0"
              },
              {
                "pattern": "\\s"
              }
            ]
          }
        },
        "payload": {
          "oneOf": [
            {
              "type": "object",
              "properties": {
                "phase": {
                  "const": "request"
                },
                "t1": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                }
              },
              "required": [
                "phase",
                "t1"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "properties": {
                "phase": {
                  "const": "response"
                },
                "t1": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "t2": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "t3": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                }
              },
              "required": [
                "phase",
                "t1",
                "t2",
                "t3"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "properties": {
                "phase": {
                  "const": "complete"
                },
                "t1": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "t2": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "t3": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                },
                "t4": {
                  "type": "string",
                  "pattern": "^(0|[1-9][0-9]{0,19})$",
                  "minLength": 1,
                  "maxLength": 20,
                  "not": {
                    "pattern": "\\s"
                  }
                }
              },
              "required": [
                "phase",
                "t1",
                "t2",
                "t3",
                "t4"
              ],
              "additionalProperties": false
            }
          ]
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "KEYFRAME_REQUEST"
        },
        "request_id": {
          "const": "0"
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "generation": {
              "type": "integer",
              "minimum": 0,
              "maximum": 4294967295
            },
            "ssrc": {
              "type": "integer",
              "minimum": 1,
              "maximum": 4294967295
            }
          },
          "required": [
            "session_id",
            "generation",
            "ssrc"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "SESSION_STOP"
        },
        "request_id": {
          "const": "0"
        },
        "payload": {
          "type": "object",
          "properties": {
            "session_id": {
              "type": "string",
              "pattern": "^[0-9a-f]{32}$",
              "minLength": 32,
              "maxLength": 32,
              "not": {
                "const": "00000000000000000000000000000000"
              }
            },
            "reason": {
              "enum": [
                "off",
                "game_end",
                "transport_lost",
                "replaced",
                "shutdown",
                "error"
              ]
            }
          },
          "required": [
            "session_id",
            "reason"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "ERROR"
        },
        "request_id": {
          "type": "string",
          "pattern": "^(0|[1-9][0-9]{0,19})$",
          "minLength": 1,
          "maxLength": 20,
          "not": {
            "pattern": "\\s"
          }
        },
        "payload": {
          "type": "object",
          "properties": {
            "code": {
              "enum": [
                "VERSION_UNSUPPORTED",
                "AUTH_FAILED",
                "PAIR_EXPIRED",
                "PAIR_USED",
                "BUSY",
                "CHANNEL_MISMATCH",
                "MALFORMED",
                "CAPABILITY_UNSUPPORTED",
                "VIDEO_UNAVAILABLE",
                "AUDIO_UNAVAILABLE",
                "PERMISSION_DENIED",
                "NETWORK_UNAVAILABLE",
                "INTERNAL"
              ]
            },
            "message": {
              "type": "string",
              "maxLength": 256
            },
            "fatal": {
              "type": "boolean"
            }
          },
          "required": [
            "code",
            "message",
            "fatal"
          ],
          "additionalProperties": false
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    },
    {
      "type": "object",
      "properties": {
        "version": {
          "const": 1
        },
        "type": {
          "const": "VIDEO_PREFERENCES"
        },
        "request_id": {
          "const": "0"
        },
        "payload": {
          "type": "object",
          "properties": {
            "width": {
              "type": "integer"
            },
            "height": {
              "type": "integer"
            },
            "fps": {
              "enum": [
                30,
                60
              ]
            }
          },
          "required": [
            "width",
            "height",
            "fps"
          ],
          "additionalProperties": false,
          "oneOf": [
            {
              "properties": {
                "width": {
                  "const": 854
                },
                "height": {
                  "const": 480
                }
              }
            },
            {
              "properties": {
                "width": {
                  "const": 1280
                },
                "height": {
                  "const": 720
                }
              }
            },
            {
              "properties": {
                "width": {
                  "const": 1920
                },
                "height": {
                  "const": 1080
                }
              }
            }
          ]
        }
      },
      "required": [
        "version",
        "type",
        "request_id",
        "payload"
      ],
      "additionalProperties": false
    }
  ]
}
)CEMUPAD_SCHEMA";
}
