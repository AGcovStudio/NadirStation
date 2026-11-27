// GENERATED 2026-10-17 by work/nav_staging/cld_train.py -- do not edit by hand.
// Daytime FY-2G cloud mask decision tree (depth 6, min 500 samples/leaf, class-balanced), trained on
// the station full-disk features of 5 scenes (2026-10-04/05) against the official NSMC
// NOMCloudClassification of the matching hourly FDI files (only pixels whose 5x5 official labels
// agree). Leave-one-scene-out on all valid daytime pixels: 82.1 % agreement, HSS 0.64; the previous
// threshold mask scored 78.6 %, HSS 0.57 on the same pixels. Returns 1 = cloud, 0 = clear.
struct CloudTreeFeatures {
    double T11, D, W, M, SD11, DMAX11, DMAX25, VISN, VSD, land, cos_sat, cos_sun;
};
inline int fy2gDaytimeCloudTree(const CloudTreeFeatures& f) {
    if (f.T11 <= 287.584) {
        if (f.M <= 17.8428) {
            if (f.DMAX25 <= 3.28722) {
                if (f.land <= 0.5) {
                    if (f.D <= 4.29056) {
                        if (f.VISN <= 28.1668) {
                            return 0;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.cos_sun <= 0.848087) {
                            return 1;
                        } else {
                            return 0;
                        }
                    }
                } else {
                    if (f.W <= -40.0817) {
                        if (f.M <= 13.2817) {
                            return 0;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.VISN <= 13.8583) {
                            return 0;
                        } else {
                            return 1;
                        }
                    }
                }
            } else {
                if (f.M <= 11.5428) {
                    if (f.VISN <= 13.9865) {
                        if (f.T11 <= 278.214) {
                            return 0;
                        } else {
                            return 0;
                        }
                    } else {
                        if (f.VISN <= 33.7348) {
                            return 0;
                        } else {
                            return 1;
                        }
                    }
                } else {
                    if (f.cos_sat <= 0.831455) {
                        if (f.T11 <= 284.444) {
                            return 1;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.land <= 0.5) {
                            return 1;
                        } else {
                            return 1;
                        }
                    }
                }
            }
        } else {
            if (f.M <= 24.5506) {
                if (f.cos_sat <= 0.842773) {
                    if (f.T11 <= 266.583) {
                        if (f.T11 <= 263.938) {
                            return 1;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.land <= 0.5) {
                            return 1;
                        } else {
                            return 1;
                        }
                    }
                } else {
                    if (f.T11 <= 285.059) {
                        if (f.land <= 0.5) {
                            return 1;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.cos_sat <= 0.956384) {
                            return 1;
                        } else {
                            return 1;
                        }
                    }
                }
            } else {
                if (f.T11 <= 274.919) {
                    if (f.T11 <= 265.306) {
                        if (f.cos_sat <= 0.650969) {
                            return 1;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.cos_sat <= 0.383064) {
                            return 1;
                        } else {
                            return 1;
                        }
                    }
                } else {
                    if (f.cos_sat <= 0.403529) {
                        if (f.D <= 4.62444) {
                            return 0;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.cos_sun <= 0.572208) {
                            return 1;
                        } else {
                            return 1;
                        }
                    }
                }
            }
        }
    } else {
        if (f.M <= 17.0661) {
            if (f.T11 <= 290.293) {
                if (f.D <= 4.81278) {
                    if (f.M <= 11.3128) {
                        if (f.cos_sat <= 0.74524) {
                            return 0;
                        } else {
                            return 0;
                        }
                    } else {
                        if (f.cos_sat <= 0.888222) {
                            return 0;
                        } else {
                            return 0;
                        }
                    }
                } else {
                    if (f.cos_sat <= 0.654014) {
                        if (f.cos_sun <= 0.32627) {
                            return 1;
                        } else {
                            return 0;
                        }
                    } else {
                        if (f.land <= 0.5) {
                            return 1;
                        } else {
                            return 0;
                        }
                    }
                }
            } else {
                if (f.VISN <= 20.1841) {
                    if (f.cos_sat <= 0.6917) {
                        if (f.D <= 3.21611) {
                            return 0;
                        } else {
                            return 0;
                        }
                    } else {
                        if (f.T11 <= 291.203) {
                            return 0;
                        } else {
                            return 0;
                        }
                    }
                } else {
                    if (f.cos_sat <= 0.699383) {
                        if (f.VISN <= 31.8682) {
                            return 0;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.D <= 2.91056) {
                            return 0;
                        } else {
                            return 0;
                        }
                    }
                }
            }
        } else {
            if (f.cos_sat <= 0.760922) {
                if (f.cos_sun <= 0.726067) {
                    if (f.cos_sat <= 0.40599) {
                        return 0;
                    } else {
                        if (f.M <= 18.1928) {
                            return 1;
                        } else {
                            return 1;
                        }
                    }
                } else {
                    if (f.D <= 4.28278) {
                        if (f.cos_sun <= 0.739159) {
                            return 0;
                        } else {
                            return 0;
                        }
                    } else {
                        if (f.M <= 18.8517) {
                            return 0;
                        } else {
                            return 1;
                        }
                    }
                }
            } else {
                if (f.T11 <= 289.926) {
                    if (f.land <= 0.5) {
                        if (f.cos_sat <= 0.951929) {
                            return 1;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.D <= 5.32278) {
                            return 0;
                        } else {
                            return 1;
                        }
                    }
                } else {
                    if (f.cos_sat <= 0.782468) {
                        if (f.M <= 20.4883) {
                            return 0;
                        } else {
                            return 1;
                        }
                    } else {
                        if (f.SD11 <= 1.02267) {
                            return 0;
                        } else {
                            return 0;
                        }
                    }
                }
            }
        }
    }
}
